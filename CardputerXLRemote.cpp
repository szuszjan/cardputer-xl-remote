// ============================================================================
// CXL REMOTE - remote desktop client for the Cardputer XL
// ============================================================================
//
// The Cardputer XL is an M5Stack Cardputer-ADV with an external 320x240
// ILI9341 on the EXT header (same wiring as github.com/szuszjan/cardputer-xl).
// This sketch turns it into a thin remote-desktop terminal for a PC running
// server/cxl_remote_server.py:
//
//   PC  --(changed screen regions as small JPEG tiles, TCP 5901)-->  ILI9341
//   PC  <--(key down/up, mouse moves/buttons/wheel, zoom commands)--  keyboard
//
// The PC does all the heavy lifting (capture, scaling to 320x240, cursor
// overlay, dirty-tile detection, JPEG encoding, zoom/viewport that follows
// the mouse). The Cardputer only receives tiles and forwards the keyboard:
//   core 0: rxTask() reads the TCP stream into two tile slots + an audio ring
//   core 1: loop() decodes tiles with JPEGDEC while the previous MCU block is
//           still going out to the panel over SPI DMA (ping-pong buffers)
//   core 0: audioTask() plays the PC's sound (24 kHz mu-law) on the speaker
//
// External ILI9341 = the remote screen. Built-in Cardputer screen = status
// HUD (mode, zoom, fps, key hints). Servers are found by UDP broadcast, and a
// 6-digit PIN printed by the server authenticates the Cardputer
// (HMAC-SHA256 challenge, the PIN itself never goes over the air).
//
// Wi-Fi: uses this sketch's own saved network, or falls back to the one
// CardputerXL saved in NVS ("cyberdeck" namespace) - flashing this sketch does
// not erase NVS, so on a unit that already ran CardputerXL it just connects.
//
// KEYS (session)
//   Typing goes straight through as real key down/up events (Shift, Ctrl,
//   Alt are real modifiers, so Ctrl+C, Ctrl+Shift+T, Alt+Tab work; on a
//   Polish layout Ctrl+Alt+A gives "a ogonek").
//   Fn + ; . , /   arrows          Fn + `      Esc
//   Fn + 1..0 - =  F1..F12         Fn + Del    Delete
//   Fn + [ ]       PgUp / PgDn     Fn + H / E  Home / End
//   Fn + I / P / M Insert / PrtSc / Menu
//   Opt (tap)      Win key         Opt + key   Win + key
//   Opt + Space or G0 button: toggle MOUSE mode
//   Opt + = / - / 0  zoom in / out / fit (0 again at fit: stretch to fill)
//   Opt + 1  next monitor
//   Opt + 9  image quality                   Opt + 8  backlight
//   Opt + Enter  full refresh                Opt + `  disconnect
//   Opt + [ / ]  volume down / up            Opt + \  sound on/off
//   Opt + 7  show / hide the mouse cursor
//   Opt + 6  frame-rate cap: MAX (none) / 30 / 20 / 15 / SRV (server's own)
//   Opt + 5  small screen: key hints <-> the server's temperature gauges
//
// HOTSPOT ("proxy") MODE - picker menu: the Cardputer stays on the main
// Wi-Fi AND runs its own access point (192.168.77.1/24) for the server, e.g.
// an Arduino UNO Q, routing that network's traffic out through the main
// Wi-Fi with NAT. The server then talks to the Cardputer over a direct link,
// and still has internet. With no main Wi-Fi it works as an offline link.
//   MOUSE mode: ; . , / move (accelerates, Shift = slow), Enter/Space = left
//   button (hold to drag), \ = right, ' = middle, [ ] = wheel. Every other
//   key still types.
//
// BUILD: FQBN esp32:esp32:m5stack_cardputer, libraries M5Cardputer (M5GFX,
// which also drives the external panel) and JPEGDEC.
// ============================================================================

#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <lgfx/v1/panel/Panel_LCD.hpp>
#include <JPEGDEC.h>
#include <mbedtls/md.h>
#include <vector>

// ---- Wiring: identical to CardputerXL ----
#define TFT_CS 5
#define TFT_RST 15
#define TFT_DC 13
#define TFT_MOSI 3
#define TFT_SCK 6
#define TFT_BL 39
#define HAPTIC_PIN 1
#define BTNA_PIN 0

constexpr uint16_t REMOTE_PORT = 5901;       // TCP session + UDP discovery
constexpr uint16_t DISCOVERY_LOCAL_PORT = 5902;
constexpr uint8_t PROTO_VER = 3;
#define CXL_REMOTE_VERSION "1.3.1"
constexpr int OUT_W = 320, OUT_H = 240;
constexpr uint32_t JPEG_MAX = 48 * 1024;     // server splits tiles to fit
constexpr uint32_t BL_HZ = 5000;
constexpr uint8_t BL_BITS = 8;

constexpr uint16_t C_BG = 0x0000;
constexpr uint16_t C_BAR = 0x0233;
constexpr uint16_t C_ACC = 0x2E7F;
constexpr uint16_t C_TXT = 0xFFFF;
constexpr uint16_t C_DIM = 0x8C71;
constexpr uint16_t C_SEL = 0x1A6B;
constexpr uint16_t C_ERR = 0xFA48;
constexpr uint16_t C_OK = 0x47E8;

// ---- External ILI9341 through M5GFX's own lgfx classes (no second graphics
// library, so no clash with M5GFX). It gets SPI2 + DMA to itself; the
// built-in screen is on SPI3. Init sequence copied from LovyanGFX's
// Panel_ILI9341 (FreeBSD licence, lovyan03) - M5GFX only ships ILI9342.
struct Panel_ILI9341_XL : public lgfx::Panel_LCD {
  Panel_ILI9341_XL() {
    _cfg.memory_width = _cfg.panel_width = 240;
    _cfg.memory_height = _cfg.panel_height = 320;
  }
  const uint8_t* getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
      0xEF, 3, 0x03, 0x80, 0x02,
      0xCF, 3, 0x00, 0xC1, 0x30,
      0xED, 4, 0x64, 0x03, 0x12, 0x81,
      0xE8, 3, 0x85, 0x00, 0x78,
      0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
      0xF7, 1, 0x20,
      0xEA, 2, 0x00, 0x00,
      0xC0, 1, 0x23,
      0xC1, 1, 0x10,
      0xC5, 2, 0x3e, 0x28,
      0xC7, 1, 0x86,
      0xB1, 2, 0x00, 0x13,
      0xF2, 1, 0x00,
      CMD_GAMMASET, 1, 0x01,
      0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,
      0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,
      0xB6, 3, 0x08, 0xC2, 0x27,
      CMD_SLPOUT, 0 + CMD_INIT_DELAY, 120,
      CMD_IDMOFF, 0,
      CMD_DISPON, 0 + CMD_INIT_DELAY, 100,
      0xFF, 0xFF,
    };
    return listno == 0 ? list0 : nullptr;
  }
};

class ExtPanel : public lgfx::LGFX_Device {
  Panel_ILI9341_XL _panel;
  lgfx::Bus_SPI _bus;
public:
  ExtPanel() {
    auto b = _bus.config();
    b.spi_host = SPI2_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;
    b.freq_read = 16000000;
    b.spi_3wire = false;
    b.use_lock = true;
    b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = TFT_SCK;
    b.pin_mosi = TFT_MOSI;
    b.pin_miso = -1;
    b.pin_dc = TFT_DC;
    _bus.config(b);
    _panel.setBus(&_bus);
    auto p = _panel.config();
    p.pin_cs = TFT_CS;
    p.pin_rst = TFT_RST;
    p.pin_busy = -1;
    p.readable = false;
    p.invert = false;
    p.rgb_order = false;
    p.dlen_16bit = false;
    p.bus_shared = false;
    _panel.config(p);
    setPanel(&_panel);
  }
};

ExtPanel tft;
M5Canvas hud(&M5Cardputer.Display);
Preferences prefs;
WiFiClient conn;
WiFiUDP udp;
JPEGDEC jpeg;

// ---- Persistent settings ----
String wifiSsid, wifiPass, serverPin;
IPAddress lastServerIp;
uint16_t lastServerPort = REMOTE_PORT;
uint8_t rotation = 3, brightness = 10, quality = 1, volume = 128;
bool soundOn = true, hotspotOn = false;
String apSsid, apPass;
const char* qualityNames[] = {"FAST", "NORMAL", "SHARP"};
// Frame-rate cap sent to the server ('L'): 0 = none, 255 = the server's setting.
const uint8_t fpsCaps[] = {0, 30, 20, 15, 255};
const char* fpsCapNames[] = {"MAX", "30", "20", "15", "SRV"};
uint8_t fpsCapIdx = 0;
// Server temperatures ('M', every 2 s), written by rxTask, drawn by drawHud().
constexpr int MAX_TEMPS = 12;
char tempNames[MAX_TEMPS][13];
int16_t tempX10[MAX_TEMPS];
volatile uint8_t tempCount = 0;
volatile uint32_t tempsAt = 0;
uint8_t hudPage = 0;  // 0 = key hints, 1 = temperature gauges

// ---- Session / HUD state ----
String serverName, statusLine = "Starting", statusLine2;
uint16_t serverScreenW = 0, serverScreenH = 0;
bool mouseMode = false, inSession = false, hudDirty = true;
uint16_t viewRatioX100 = 0;
uint8_t zoomIdx = 0, zoomCount = 1, monitorNo = 1;
bool stretchOn = false, audioStreaming = false, cursorShown = true;
uint32_t statFrames = 0, statBytes = 0, statT0 = 0;
float fps = 0, kbps = 0;
String lastKeyLabel;
uint32_t hapticOffAt = 0, hudLastDraw = 0;

// ============================================================================
// Small hardware helpers
// ============================================================================
void haptic(uint16_t ms = 20) { digitalWrite(HAPTIC_PIN, HIGH); hapticOffAt = millis() + ms; }
void serviceHaptic() { if (hapticOffAt && (int32_t)(millis() - hapticOffAt) >= 0) { digitalWrite(HAPTIC_PIN, LOW); hapticOffAt = 0; } }
void applyBrightness() {
  ledcWrite(TFT_BL, (uint16_t(brightness) * 255U) / 10U);
  M5Cardputer.Display.setBrightness(40 + brightness * 10);
}

void saveSettings() {
  prefs.begin("cxlremote", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.putString("pin", serverPin);
  prefs.putUInt("srv_ip", (uint32_t)lastServerIp);
  prefs.putUShort("srv_port", lastServerPort);
  prefs.putUChar("bright", brightness);
  prefs.putUChar("quality", quality);
  prefs.putUChar("fpscap", fpsCapIdx);
  prefs.putUChar("hudpage", hudPage);
  prefs.putUChar("volume", volume);
  prefs.putBool("sound", soundOn);
  prefs.putBool("hotspot", hotspotOn);
  prefs.putString("ap_ssid", apSsid);
  prefs.putString("ap_pass", apPass);
  prefs.end();
}

void loadSettings() {
  uint8_t savedBright = 0;
  if (prefs.begin("cxlremote", true)) {
    wifiSsid = prefs.getString("ssid", "");
    wifiPass = prefs.getString("pass", "");
    serverPin = prefs.getString("pin", "");
    lastServerIp = IPAddress(prefs.getUInt("srv_ip", 0));
    lastServerPort = prefs.getUShort("srv_port", REMOTE_PORT);
    savedBright = prefs.getUChar("bright", 0);
    quality = min<uint8_t>(prefs.getUChar("quality", 1), 2);
    fpsCapIdx = min<uint8_t>(prefs.getUChar("fpscap", 0), 4);
    hudPage = min<uint8_t>(prefs.getUChar("hudpage", 0), 1);
    volume = prefs.getUChar("volume", volume);
    soundOn = prefs.getBool("sound", soundOn);
    hotspotOn = prefs.getBool("hotspot", false);
    apSsid = prefs.getString("ap_ssid", "");
    apPass = prefs.getString("ap_pass", "");
    prefs.end();
  }
  // CardputerXL's own settings: Wi-Fi it already knows, panel rotation and
  // brightness the user picked there.
  if (prefs.begin("cyberdeck", true)) {
    if (wifiSsid.isEmpty()) {
      wifiSsid = prefs.getString("wifi_ssid", "");
      wifiPass = prefs.getString("wifi_pass", "");
    }
    rotation = prefs.getUChar("rotation", 3);
    if (!savedBright) savedBright = prefs.getUChar("brightness", 10);
    prefs.end();
  }
  if (rotation != 1 && rotation != 3) rotation = 3;
  brightness = constrain(savedBright ? savedBright : 10, 1, 10);
}

// ============================================================================
// Keyboard: raw physical key matrix as a 56-bit mask (bit = row * 14 + col)
// ============================================================================
constexpr int KEY_COLS = 14, KEY_COUNT = 56;
constexpr int IDX_FN = 2 * 14 + 0, IDX_SHIFT = 2 * 14 + 1, IDX_CTRL = 3 * 14 + 0, IDX_OPT = 3 * 14 + 1, IDX_ALT = 3 * 14 + 2;
inline uint64_t kbit(int i) { return 1ULL << i; }
inline uint8_t baseAt(int i) { return (uint8_t)_key_value_map[i / KEY_COLS][i % KEY_COLS].value_first; }
inline uint8_t shiftedAt(int i) { return (uint8_t)_key_value_map[i / KEY_COLS][i % KEY_COLS].value_second; }
inline bool isModifierIdx(int i) { return i == IDX_FN || i == IDX_SHIFT || i == IDX_CTRL || i == IDX_OPT || i == IDX_ALT; }

uint64_t readKeyMask() {
  M5Cardputer.update();
  // The ADV's TCA8418 hands out one FIFO event per update(); drain the rest
  // so a burst of keys during a long JPEG decode is not processed late.
  for (int i = 0; i < 10; ++i) M5Cardputer.Keyboard.updateKeyList();
  uint64_t m = 0;
  for (const auto& p : M5Cardputer.Keyboard.keyList()) m |= kbit(p.y * KEY_COLS + p.x);
  return m;
}

bool btnAWasDown = false;
bool btnAPressed() {
  bool down = digitalRead(BTNA_PIN) == LOW;
  bool edge = down && !btnAWasDown;
  btnAWasDown = down;
  return edge;
}

// One key-press edge for menus / text entry.
struct UiKey { int idx = -1; uint8_t base = 0, shifted = 0; bool fn = false, shift = false; };
uint64_t uiPrevMask = 0;
UiKey uiPoll() {
  uint64_t cur = readKeyMask(), down = cur & ~uiPrevMask;
  uiPrevMask = cur;
  UiKey k;
  k.fn = cur & kbit(IDX_FN);
  k.shift = cur & kbit(IDX_SHIFT);
  for (int i = 0; i < KEY_COUNT; ++i) {
    if (!(down & kbit(i)) || isModifierIdx(i)) continue;
    k.idx = i; k.base = baseAt(i); k.shifted = shiftedAt(i);
    break;
  }
  return k;
}

// ============================================================================
// Built-in screen HUD
// ============================================================================
void setStatus(const String& a, const String& b = "") { statusLine = a; statusLine2 = b; hudDirty = true; }

void drawTempGauges();
void drawHud() {
  hud.fillScreen((uint16_t)TFT_BLACK);
  hud.fillRect(0, 0, 240, 14, (uint16_t)0x0233);
  hud.setTextSize(1);
  hud.setTextColor((uint16_t)TFT_WHITE);
  hud.setCursor(4, 3); hud.print("CXL REMOTE");
  int bat = M5Cardputer.Power.getBatteryLevel();
  String right = (WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) + "dBm  " : String("no wifi  ")) + String(bat) + "%";
  hud.setCursor(236 - right.length() * 6, 3); hud.print(right);

  if (!inSession) {
    hud.setTextColor((uint16_t)0x2E7F); hud.setTextSize(2);
    hud.setCursor(4, 24); hud.print(statusLine.substring(0, 19));
    hud.setTextSize(1); hud.setTextColor((uint16_t)TFT_LIGHTGREY);
    hud.setCursor(4, 48); hud.print(statusLine2.substring(0, 39));
    if (WiFi.status() == WL_CONNECTED) {
      hud.setCursor(4, 64); hud.print("Wi-Fi: " + WiFi.SSID().substring(0, 30));
      hud.setCursor(4, 76); hud.print("IP:    " + WiFi.localIP().toString());
    }
    if (hotspotOn) {
      hud.setTextColor((uint16_t)0xFC60);
      hud.setCursor(4, 88); hud.print(("Hotspot: " + apSsid).substring(0, 39));
      hud.setCursor(4, 100); hud.print(("Pass: " + apPass + "  clients: " + String(WiFi.softAPgetStationNum())).substring(0, 39));
    }
    hud.setTextColor((uint16_t)0x8C71);
    hud.setCursor(4, 112); hud.print("Use the big screen. ` = back");
    hud.setCursor(4, 124); hud.print("PC: run CXLRemoteServer.exe");
    hud.pushSprite(0, 0);
    return;
  }

  hud.setTextColor((uint16_t)TFT_LIGHTGREY);
  hud.setCursor(4, 19); hud.print((serverName + "  " + String(serverScreenW) + "x" + String(serverScreenH)).substring(0, 30));
  // Hottest CPU zone (or hottest zone at all), always visible next to the resolution.
  {
    uint8_t n = tempCount;
    int best = -1, bestAny = -1;
    for (int i = 0; i < n && i < MAX_TEMPS; ++i) {
      if (bestAny < 0 || tempX10[i] > tempX10[bestAny]) bestAny = i;
      if (!strncmp(tempNames[i], "CPU", 3) && (best < 0 || tempX10[i] > tempX10[best])) best = i;
    }
    if (best < 0) best = bestAny;
    if (best >= 0 && millis() - tempsAt < 6000) {
      float t = tempX10[best] / 10.0f;
      uint16_t c = t < 60 ? (uint16_t)0x07E0 : t < 75 ? (uint16_t)0xFFE0 : t < 90 ? (uint16_t)0xFD20 : (uint16_t)0xF800;
      hud.setTextColor(c);
      hud.setCursor(190, 19); hud.printf("CPU%3.0fC", t);
      hud.setTextColor((uint16_t)TFT_LIGHTGREY);
    }
  }

  uint16_t badge = mouseMode ? (uint16_t)0xFC60 : (uint16_t)0x2589;
  hud.fillRoundRect(4, 31, 120, 24, 4, badge);
  hud.setTextSize(2); hud.setTextColor((uint16_t)TFT_BLACK);
  hud.setCursor(mouseMode ? 34 : 16, 36); hud.print(mouseMode ? "MOUSE" : "KEYBOARD");

  hud.setTextColor((uint16_t)TFT_WHITE);
  String zoom = zoomIdx == 0 ? String(stretchOn ? "STRETCH" : "FIT") : String("1:") + String(viewRatioX100 / 100.0f, viewRatioX100 % 100 ? 1 : 0);
  hud.setCursor(134, 36); hud.print(zoom);
  hud.setTextSize(1);
  hud.setTextColor((uint16_t)0x8C71);
  hud.setCursor(222, 33); hud.print("MON");
  hud.setCursor(228, 44); hud.setTextColor((uint16_t)TFT_WHITE); hud.print(monitorNo);

  hud.setTextColor((uint16_t)TFT_LIGHTGREY);
  hud.setCursor(4, 61);
  hud.printf("%4.1f/%s fps %4.0fKB/s Q:%s", fps, fpsCapNames[fpsCapIdx], kbps, qualityNames[quality]);
  hud.setCursor(4, 72); hud.setTextColor((uint16_t)0x8C71);
  String snd = !soundOn ? String("MUTE") : (audioStreaming ? "VOL " + String(volume * 100 / 255) + "%" : String("no audio"));
  hud.print((snd + "  key: " + lastKeyLabel).substring(0, 39));

  hud.setTextColor((uint16_t)0x2E7F);
  const char* const* lines;
  static const char* keyHints[] = {"Fn+;.,/ arrows  Fn+` Esc  Fn+1-= F1-12",
                                   "Fn+Del Del Fn+[] PgUp/Dn Opt+[]\\ sound",
                                   "Opt+Spc/G0 mouse Opt+6 fps Opt+5 temps",
                                   "Opt+0 fit Opt+7 cursor Opt+9 q Opt+` quit"};
  static const char* mouseHints[] = {";.,/ move (Shift slow)  Ent/Spc left",
                                     "\\ right  ' middle  [ ] wheel  hold=drag",
                                     "Opt+Spc/G0 keybd  Opt+-= zoom  Opt+0 fit",
                                     "other keys still type  Opt+` quit"};
  lines = mouseMode ? mouseHints : keyHints;
  if (hudPage == 1) {
    drawTempGauges();
  } else {
    for (int i = 0; i < 4; ++i) { hud.setCursor(2, 87 + i * 12); hud.print(lines[i]); }
  }
  hud.pushSprite(0, 0);
}

// Hotspot mode keeps the radio on the AP's channel: the stock auto-reconnect
// would rescan every channel back to back whenever the main Wi-Fi drops,
// starving the hotspot (100 ms+ stalls, ~0 fps). Instead: one targeted retry
// (known channel + BSSID, no scan) every 30 s, a full one every 60 s when the
// main network was never seen. NAT is (re)enabled as soon as it's up.
uint32_t staRetryAt = 0;
bool staWasUp = false, staKnown = false;
uint8_t staChannel = 0, staBssid[6];
void serviceWifi() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !staWasUp) {
    staChannel = WiFi.channel();
    memcpy(staBssid, WiFi.BSSID(), 6);
    staKnown = true;
    if (hotspotOn) WiFi.AP.enableNAPT(true);
    hudDirty = true;
  }
  if (!up && staWasUp) { staRetryAt = millis() + 5000; hudDirty = true; }
  staWasUp = up;
  if (!hotspotOn || up || wifiSsid.isEmpty() || (int32_t)(millis() - staRetryAt) < 0) return;
  if (staKnown) WiFi.begin(wifiSsid.c_str(), wifiPass.c_str(), staChannel, staBssid);
  else WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  staRetryAt = millis() + (staKnown ? 30000 : 60000);
}

// Two columns x 6 rows of mini gauges: label, bar 30..95 C (the UNO Q's CPU
// starts throttling at 90 C), value.
void drawTempGauges() {
  uint8_t n = tempCount;
  if (!n || millis() - tempsAt > 6000) {
    hud.setTextColor((uint16_t)0x8C71);
    hud.setCursor(4, 100); hud.print(n ? "temperatures: no update" : "no temperature sensors on this PC");
    hud.setCursor(4, 112); hud.print("Opt+5: back to key hints");
    return;
  }
  for (int i = 0; i < n && i < MAX_TEMPS; ++i) {
    int col = i / 6, row = i % 6;
    int x = 2 + col * 120, y = 86 + row * 8;
    float t = tempX10[i] / 10.0f;
    uint16_t c = t < 60 ? (uint16_t)0x07E0 : t < 75 ? (uint16_t)0xFFE0 : t < 90 ? (uint16_t)0xFD20 : (uint16_t)0xF800;
    hud.setTextColor((uint16_t)TFT_LIGHTGREY);
    hud.setCursor(x, y); hud.print(tempNames[i]);
    int w = constrain((int)((t - 30.0f) / 65.0f * 52.0f), 1, 52);
    hud.fillRect(x + 32, y + 1, 52, 5, (uint16_t)0x2104);
    hud.fillRect(x + 32, y + 1, w, 5, c);
    hud.setTextColor(c);
    hud.setCursor(x + 88, y);
    hud.printf("%2.0fC", t);
  }
}

void serviceHud(bool force = false) {
  serviceWifi();
  uint32_t now = millis();
  if (force || (hudDirty && now - hudLastDraw > 120) || now - hudLastDraw > 1000) {
    drawHud();
    hudDirty = false;
    hudLastDraw = millis();
  }
}

// ============================================================================
// External-panel menu UI (setup screens)
// ============================================================================
void uiHeader(const char* title) {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, OUT_W, 24, C_BAR);
  tft.setTextSize(2); tft.setTextColor(C_TXT, C_BAR);
  tft.setCursor(6, 5); tft.print(title);
}
void uiFooter(const char* text) {
  tft.fillRect(0, OUT_H - 14, OUT_W, 14, C_BAR);
  tft.setTextSize(1); tft.setTextColor(C_TXT, C_BAR);
  tft.setCursor(4, OUT_H - 11); tft.print(text);
}
void uiText(int x, int y, const String& s, uint16_t color, uint8_t size = 2) {
  tft.setTextSize(size); tft.setTextColor(color, C_BG);
  tft.setCursor(x, y); tft.print(s);
}
void uiMessage(const char* title, const String& l1, const String& l2 = "", uint16_t color = C_TXT, const char* footer = nullptr) {
  uiHeader(title);
  uiText(10, 50, l1, color);
  if (l2.length()) uiText(10, 80, l2, C_DIM, 1);
  if (footer) uiFooter(footer);
  setStatus(title, l1);
  serviceHud(true);
}
void waitAnyKey(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) { if (uiPoll().idx >= 0 || btnAPressed()) return; serviceHaptic(); delay(10); }
}

void uiListRow(int row, const String& s, bool selected) {
  int y = 30 + row * 21;
  uint16_t bg = selected ? C_SEL : C_BG;
  tft.fillRect(0, y - 2, OUT_W, 20, bg);
  tft.setTextSize(2); tft.setTextColor(selected ? C_TXT : C_DIM, bg);
  tft.setCursor(8, y); tft.print(s.substring(0, 25));
}
constexpr int LIST_ROWS = 9;
void uiDrawList(const std::vector<String>& items, int sel, int& top) {
  if (sel < top) top = sel;
  if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
  for (int r = 0; r < LIST_ROWS; ++r) {
    int i = top + r;
    if (i < (int)items.size()) uiListRow(r, items[i], i == sel);
    else tft.fillRect(0, 28 + r * 21, OUT_W, 20, C_BG);
  }
}

// Returns item index, or -1 for back, or -(100 + hotkey) for a hotkey letter.
int listMenu(const char* title, const std::vector<String>& items, const char* footer, const char* hotkeys, int sel = 0) {
  uiHeader(title); uiFooter(footer);
  int top = 0;
  bool dirty = true;
  while (true) {
    if (dirty) { uiDrawList(items, sel, top); dirty = false; }
    serviceHud(); serviceHaptic();
    UiKey k = uiPoll();
    if (k.idx < 0) { delay(8); continue; }
    if (k.base == '`') return -1;
    if (k.base == KEY_ENTER && !items.empty()) return sel;
    if (k.base == ';' && sel > 0) { --sel; dirty = true; continue; }
    if (k.base == '.' && sel + 1 < (int)items.size()) { ++sel; dirty = true; continue; }
    if (hotkeys && k.base && strchr(hotkeys, k.base)) return -(100 + k.base);
  }
}

bool textInput(const char* title, const String& label, String& value, bool secret, size_t maxLen) {
  String v = value;
  bool show = !secret, dirty = true;
  uiHeader(title);
  uiText(10, 40, label.substring(0, 50), C_DIM, 1);
  uiFooter(secret ? "ENTER ok  DEL erase  TAB show/hide  FN+` cancel" : "ENTER ok  DEL erase  FN+` cancel");
  setStatus(title, label); serviceHud(true);
  while (true) {
    if (dirty) {
      tft.fillRect(8, 56, 304, 30, C_SEL);
      tft.drawRect(8, 56, 304, 30, C_ACC);
      String shown = v;
      if (!show) { shown = ""; for (size_t i = 0; i < v.length(); ++i) shown += '*'; }
      if (shown.length() > 24) shown = shown.substring(shown.length() - 24);
      tft.setTextSize(2); tft.setTextColor(C_TXT, C_SEL);
      tft.setCursor(14, 64); tft.print(shown); tft.print('_');
      dirty = false;
    }
    serviceHud(); serviceHaptic();
    UiKey k = uiPoll();
    if (k.idx < 0) { delay(8); continue; }
    if (k.base == KEY_ENTER) { value = v; return true; }
    if (k.base == '`' && k.fn) return false;
    if (k.base == KEY_BACKSPACE) { if (v.length()) v.remove(v.length() - 1); dirty = true; continue; }
    if (k.base == KEY_TAB) { if (secret) show = !show; dirty = true; continue; }
    char c = (char)(k.shift ? k.shifted : k.base);
    if (c >= 32 && c < 127 && v.length() < maxLen) { v += c; dirty = true; }
  }
}

// ============================================================================
// Wi-Fi
// ============================================================================
const IPAddress AP_IP(192, 168, 77, 1);

void ensureApCredentials() {
  if (apSsid.isEmpty()) {
    char id[8];
    snprintf(id, sizeof(id), "%04X", (uint16_t)(ESP.getEfuseMac() >> 32));
    apSsid = String("CXL-Remote-") + id;
  }
  if (apPass.length() < 8) {
    const char* abc = "abcdefghjkmnpqrstuvwxyz23456789";
    apPass = "";
    for (int i = 0; i < 10; ++i) apPass += abc[esp_random() % 31];
  }
}

// Own access point for the server, with NAT out through the main Wi-Fi.
void startHotspot() {
  ensureApCredentials();
  if (!(WiFi.getMode() & WIFI_AP)) WiFi.mode(WIFI_AP_STA);
  IPAddress dns = WiFi.status() == WL_CONNECTED ? WiFi.dnsIP() : IPAddress(1, 1, 1, 1);
  if (dns == IPAddress((uint32_t)0)) dns = IPAddress(1, 1, 1, 1);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0), IPAddress(192, 168, 77, 10), dns);
  // The AP follows the main network's channel once the station is connected.
  WiFi.softAP(apSsid.c_str(), apPass.c_str(), 1, 0, 4);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  WiFi.AP.enableNAPT(WiFi.status() == WL_CONNECTED);
  staRetryAt = millis() + 30000;
  udp.begin(DISCOVERY_LOCAL_PORT);
}

void stopHotspot() {
  WiFi.AP.enableNAPT(false);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  if (WiFi.status() != WL_CONNECTED && !wifiSsid.isEmpty()) WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}

bool connectWifi() {
  if (wifiSsid.isEmpty()) {
    if (hotspotOn) { startHotspot(); return true; }  // offline link only
    return false;
  }
  uiMessage("WI-FI", "Connecting to", "", C_DIM, "` cancel -> Wi-Fi setup");
  uiText(10, 76, wifiSsid.substring(0, 25), C_ACC);
  setStatus("Wi-Fi...", wifiSsid);
  WiFi.mode(hotspotOn ? WIFI_AP_STA : WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(!hotspotOn);  // hotspot mode retries itself, see serviceWifi()
  WiFi.setSleep(false);  // modem sleep adds ~100 ms of input latency
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uint32_t t0 = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    // In hotspot mode a missing main network is not fatal: the AP still
    // gives the server a direct (offline) link to the Cardputer.
    if (millis() - t0 > 20000) { if (hotspotOn) { startHotspot(); return true; } return false; }
    UiKey k = uiPoll();
    if (k.base == '`') { if (hotspotOn) { startHotspot(); return true; } return false; }
    if ((millis() - t0) / 400 != (uint32_t)dots) {
      dots = (millis() - t0) / 400;
      tft.fillRect(10, 110, 300, 8, C_BG);
      for (int i = 0; i < dots % 25; ++i) tft.fillRect(10 + i * 12, 110, 8, 8, C_ACC);
    }
    serviceHud();
    delay(20);
  }
  udp.begin(DISCOVERY_LOCAL_PORT);
  if (hotspotOn) startHotspot();
  return true;
}

void wifiSetup() {
  while (true) {
    uiMessage("WI-FI SETUP", "Scanning...");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks();
    std::vector<String> names, labels;
    std::vector<bool> open;
    for (int i = 0; i < n; ++i) {
      String s = WiFi.SSID(i);
      if (s.isEmpty()) continue;
      bool dup = false;
      for (auto& e : names) if (e == s) dup = true;
      if (dup) continue;
      names.push_back(s);
      open.push_back(WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
      String l = s.substring(0, 19);
      while (l.length() < 20) l += ' ';
      labels.push_back(l + String(WiFi.RSSI(i)));
    }
    WiFi.scanDelete();
    if (names.empty()) labels.push_back("(no networks found)");
    setStatus("Wi-Fi setup", String(names.size()) + " networks");
    int sel = listMenu("WI-FI SETUP", labels, ";/. pick  ENTER choose  R rescan  ` back", "rR");
    if (sel == -1) return;
    if (sel <= -100 || names.empty()) continue;
    String pass = names[sel] == wifiSsid ? wifiPass : String("");
    if (!open[sel] && !textInput("WI-FI PASSWORD", names[sel], pass, true, 63)) continue;
    wifiSsid = names[sel];
    wifiPass = open[sel] ? String("") : pass;
    saveSettings();
    return;
  }
}

// ============================================================================
// Server discovery (UDP broadcast "CXLR?" -> "CXLR!" + port + hostname)
// ============================================================================
struct ServerInfo { IPAddress ip; uint16_t port; String name; };
std::vector<ServerInfo> servers;

void discoveryBroadcast() {
  const uint8_t q[] = {'C', 'X', 'L', 'R', '?'};
  udp.beginPacket(IPAddress(255, 255, 255, 255), REMOTE_PORT); udp.write(q, sizeof(q)); udp.endPacket();
  if (WiFi.status() == WL_CONNECTED) { udp.beginPacket(WiFi.broadcastIP(), REMOTE_PORT); udp.write(q, sizeof(q)); udp.endPacket(); }
  if (hotspotOn) { udp.beginPacket(IPAddress(192, 168, 77, 255), REMOTE_PORT); udp.write(q, sizeof(q)); udp.endPacket(); }
}

bool discoveryPoll() {
  bool changed = false;
  while (int len = udp.parsePacket()) {
    uint8_t b[64];
    int n = udp.read(b, sizeof(b) - 1);
    if (n < 7 || memcmp(b, "CXLR!", 5) != 0) continue;
    ServerInfo s;
    s.ip = udp.remoteIP();
    s.port = b[5] | (b[6] << 8);
    b[n] = 0;
    s.name = String((const char*)b + 7);
    bool found = false;
    for (auto& e : servers) if (e.ip == s.ip && e.port == s.port) { found = true; if (e.name != s.name) { e.name = s.name; changed = true; } }
    if (!found) { servers.push_back(s); changed = true; }
    (void)len;
  }
  return changed;
}

// ============================================================================
// Session connection + handshake
// ============================================================================
enum ConnResult { CONN_OK, CONN_FAIL, CONN_BADPIN };
String connError;

bool readExact(uint8_t* b, size_t n, uint32_t timeoutMs) {
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < n) {
    int avail = conn.available();
    if (avail > 0) {
      int r = conn.read(b + got, min<size_t>(avail, n - got));
      if (r > 0) got += r;
      continue;
    }
    if (!conn.connected()) return false;
    if (millis() - t0 > timeoutMs) return false;
    delay(1);
  }
  return true;
}

ConnResult remoteConnect(IPAddress ip, uint16_t port) {
  connError = "";
  conn.stop();
  if (!conn.connect(ip, port, 3000)) { connError = "No answer from " + ip.toString(); return CONN_FAIL; }
  conn.setNoDelay(true);
  uint8_t hello[21];
  if (!readExact(hello, sizeof(hello), 3000) || memcmp(hello, "CXLS", 4) != 0) { connError = "Not a CXL Remote server"; conn.stop(); return CONN_FAIL; }
  if (hello[4] != PROTO_VER) { connError = "Server protocol v" + String(hello[4]) + ", need v" + String(PROTO_VER); conn.stop(); return CONN_FAIL; }

  uint8_t mac[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t*)serverPin.c_str(), serverPin.length(), hello + 5, 16, mac);
  uint8_t reply[45] = {'C', 'X', 'L', 'R', PROTO_VER, OUT_W & 0xFF, OUT_W >> 8, OUT_H & 0xFF, OUT_H >> 8,
                       (uint8_t)(JPEG_MAX), (uint8_t)(JPEG_MAX >> 8), (uint8_t)(JPEG_MAX >> 16), (uint8_t)(JPEG_MAX >> 24)};
  memcpy(reply + 13, mac, 32);
  conn.write(reply, sizeof(reply));

  uint8_t verdict;
  if (!readExact(&verdict, 1, 4000)) { connError = "Handshake timed out"; conn.stop(); return CONN_FAIL; }
  if (verdict == 'X') {
    uint8_t reason = 0; readExact(&reason, 1, 500);
    conn.stop();
    if (reason == 1) { connError = "Wrong PIN"; return CONN_BADPIN; }
    connError = "Server refused (" + String(reason) + ")";
    return CONN_FAIL;
  }
  uint8_t info[5];
  if (verdict != 'O' || !readExact(info, 5, 2000)) { connError = "Bad handshake"; conn.stop(); return CONN_FAIL; }
  serverScreenW = info[0] | (info[1] << 8);
  serverScreenH = info[2] | (info[3] << 8);
  char name[64] = {0};
  uint8_t nl = min<uint8_t>(info[4], 63);
  if (nl && !readExact((uint8_t*)name, nl, 2000)) { connError = "Bad handshake"; conn.stop(); return CONN_FAIL; }
  if (info[4] > nl) { uint8_t skip[256]; readExact(skip, info[4] - nl, 500); }
  serverName = name;
  return CONN_OK;
}

// ============================================================================
// Session: outgoing input messages  [type][len][payload]
// ============================================================================
void sendMsg(uint8_t type, const uint8_t* payload, uint8_t len) {
  uint8_t buf[2 + 32];
  buf[0] = type; buf[1] = len;
  if (len) memcpy(buf + 2, payload, len);
  conn.write(buf, 2 + len);
}
void sendKey(const char* name, bool down) {
  uint8_t p[24];
  size_t n = min<size_t>(strlen(name), 22);
  p[0] = down;
  memcpy(p + 1, name, n);
  sendMsg('K', p, 1 + n);
  if (down) { lastKeyLabel = name; hudDirty = true; }
}
void sendButton(uint8_t b, bool down) { uint8_t p[2] = {b, (uint8_t)down}; sendMsg('B', p, 2); }
void sendWheel(int8_t v, int8_t h) { uint8_t p[2] = {(uint8_t)v, (uint8_t)h}; sendMsg('W', p, 2); }
void sendMove(int16_t dx8, int16_t dy8) { uint8_t p[4] = {(uint8_t)dx8, (uint8_t)(dx8 >> 8), (uint8_t)dy8, (uint8_t)(dy8 >> 8)}; sendMsg('M', p, 4); }
void sendZoom(int8_t z) { uint8_t p[1] = {(uint8_t)z}; sendMsg('Z', p, 1); }
void sendQuality() { uint8_t p[1] = {quality}; sendMsg('Q', p, 1); }
void sendFpsCap() { uint8_t p[1] = {fpsCaps[fpsCapIdx]}; sendMsg('L', p, 1); }

// ============================================================================
// Session: keyboard -> actions. Each physical key remembers what its press
// sent, so its release sends the matching "up" even if Fn/Opt/mode changed.
// ============================================================================
enum ActKind : uint8_t { A_NONE, A_KEY, A_MOVE, A_BTN, A_SCROLL };
struct Act { ActKind kind; uint8_t arg; const char* name; uint32_t downAt, lastRep; };
Act acts[KEY_COUNT];
uint64_t sessPrevMask = 0;
bool optUsed = false, optWinDown = false, exitRequested = false;
float moveAccX = 0, moveAccY = 0;
uint32_t lastMoveSend = 0, lastMoveTick = 0;
char charNames[KEY_COUNT][2];

const char* baseName(uint8_t c) {
  switch (c) {
    case KEY_BACKSPACE: return "backspace";
    case KEY_TAB: return "tab";
    case KEY_ENTER: return "enter";
    case ' ': return "space";
    case KEY_LEFT_SHIFT: return "shift";
    case KEY_LEFT_CTRL: return "ctrl";
    case KEY_LEFT_ALT: return "alt";
  }
  return nullptr;
}
const char* fnName(uint8_t c) {
  switch (c) {
    case ';': return "up";      case '.': return "down";
    case ',': return "left";    case '/': return "right";
    case '`': return "esc";     case KEY_BACKSPACE: return "delete";
    case '1': return "f1";  case '2': return "f2";  case '3': return "f3";  case '4': return "f4";
    case '5': return "f5";  case '6': return "f6";  case '7': return "f7";  case '8': return "f8";
    case '9': return "f9";  case '0': return "f10"; case '-': return "f11"; case '=': return "f12";
    case '[': return "page_up"; case ']': return "page_down";
    case 'h': return "home";    case 'e': return "end";
    case 'i': return "insert";  case 'p': return "print_screen";
    case 'm': return "menu";    case KEY_ENTER: return "enter";
    case ' ': return "space";   case KEY_TAB: return "tab";
  }
  return nullptr;
}
bool isRepeatable(const char* n) { return strcmp(n, "shift") && strcmp(n, "ctrl") && strcmp(n, "alt") && strcmp(n, "cmd"); }

void toggleMouseMode() { mouseMode = !mouseMode; haptic(mouseMode ? 35 : 20); hudDirty = true; }
void cycleBrightness() { brightness = brightness >= 10 ? 2 : min(10, brightness + 2); applyBrightness(); saveSettings(); }

// Opt + key: local command? Returns true when handled.
void sendSoundWanted();
bool optCommand(uint8_t c) {
  switch (c) {
    case '`': exitRequested = true; return true;
    case ' ': toggleMouseMode(); return true;
    case '=': sendZoom(1); haptic(); return true;
    case '-': sendZoom(-1); haptic(); return true;
    case '0': sendZoom(0); haptic(); return true;
    case '1': sendMsg('N', nullptr, 0); haptic(); return true;
    case '9': quality = (quality + 1) % 3; sendQuality(); saveSettings(); hudDirty = true; haptic(); return true;
    case '8': cycleBrightness(); haptic(); return true;
    case '7': { uint8_t show = !cursorShown; sendMsg('C', &show, 1); haptic(); return true; }
    case '6': fpsCapIdx = (fpsCapIdx + 1) % 5; sendFpsCap(); saveSettings(); hudDirty = true; haptic(); return true;
    case '5': hudPage ^= 1; saveSettings(); hudDirty = true; haptic(); return true;
    case KEY_ENTER: sendMsg('R', nullptr, 0); haptic(); return true;
    case '[': case ']':
      volume = c == '[' ? (volume > 16 ? volume - 16 : 0) : min(255, volume + 16);
      M5Cardputer.Speaker.setVolume(volume); saveSettings(); hudDirty = true; return true;
    case '\\': soundOn = !soundOn; sendSoundWanted(); saveSettings(); hudDirty = true; haptic(); return true;
  }
  return false;
}

void pressKey(int i, uint64_t cur) {
  uint8_t c = baseAt(i);
  Act& a = acts[i];
  a = {A_NONE, 0, nullptr, millis(), millis()};
  bool fn = cur & kbit(IDX_FN), opt = cur & kbit(IDX_OPT);
  if (i == IDX_FN) return;
  if (i == IDX_OPT) { optUsed = false; optWinDown = false; return; }
  bool modifier = (i == IDX_SHIFT || i == IDX_CTRL || i == IDX_ALT);

  if (opt && !modifier) {
    optUsed = true;
    if (optCommand(c)) return;
    if (!optWinDown) { sendKey("cmd", true); optWinDown = true; }
  }
  const char* name = nullptr;
  if (fn && !modifier) {
    name = fnName(c);
    if (!name) return;
  } else if (mouseMode && !modifier) {
    switch (c) {
      case ';': a.kind = A_MOVE; a.arg = 0; return;
      case '.': a.kind = A_MOVE; a.arg = 1; return;
      case ',': a.kind = A_MOVE; a.arg = 2; return;
      case '/': a.kind = A_MOVE; a.arg = 3; return;
      case KEY_ENTER: case ' ': a.kind = A_BTN; a.arg = 0; sendButton(0, true); lastKeyLabel = "left btn"; hudDirty = true; return;
      case '\\': a.kind = A_BTN; a.arg = 1; sendButton(1, true); lastKeyLabel = "right btn"; hudDirty = true; return;
      case '\'': a.kind = A_BTN; a.arg = 2; sendButton(2, true); lastKeyLabel = "middle btn"; hudDirty = true; return;
      case '[': a.kind = A_SCROLL; a.arg = 1; sendWheel(1, 0); return;
      case ']': a.kind = A_SCROLL; a.arg = 0; sendWheel(-1, 0); return;
    }
  }
  if (!name) name = baseName(c);
  if (!name) { charNames[i][0] = (char)c; charNames[i][1] = 0; name = charNames[i]; }
  a.kind = A_KEY; a.name = name;
  sendKey(name, true);
}

void releaseKey(int i) {
  if (i == IDX_OPT) {
    if (optWinDown) sendKey("cmd", false);
    else if (!optUsed) { sendKey("cmd", true); sendKey("cmd", false); }
    optWinDown = false;
    return;
  }
  Act& a = acts[i];
  if (a.kind == A_KEY) sendKey(a.name, false);
  else if (a.kind == A_BTN) sendButton(a.arg, false);
  a.kind = A_NONE;
}

void releaseAll() {
  for (int i = 0; i < KEY_COUNT; ++i) if (sessPrevMask & kbit(i)) releaseKey(i);
  sessPrevMask = 0;
}

void serviceInput() {
  uint64_t cur = readKeyMask();
  uint64_t down = cur & ~sessPrevMask, up = sessPrevMask & ~cur;
  // Releases first (with Opt last), so a fast roll from one key to the next
  // never leaves two keys or a stale Win modifier held on the PC.
  for (int i = 0; i < KEY_COUNT; ++i) if ((up & kbit(i)) && i != IDX_OPT) releaseKey(i);
  if (up & kbit(IDX_OPT)) releaseKey(IDX_OPT);
  if (down & kbit(IDX_OPT)) pressKey(IDX_OPT, cur);
  for (int i = 0; i < KEY_COUNT; ++i) if ((down & kbit(i)) && i != IDX_OPT) pressKey(i, cur);
  sessPrevMask = cur;
  if (btnAPressed()) toggleMouseMode();
  // Taken only after pressKey() stamped downAt: an earlier "now" would make
  // now - downAt wrap around and fire the auto-repeat instantly.
  uint32_t now = millis();

  // Typematic repeat: injected key-downs are not auto-repeated by the PC.
  float vx = 0, vy = 0;
  uint32_t moveSince = 0;
  for (int i = 0; i < KEY_COUNT; ++i) {
    if (!(cur & kbit(i))) continue;
    Act& a = acts[i];
    if (a.kind == A_KEY && isRepeatable(a.name) && now - a.downAt > 450 && now - a.lastRep >= 40) {
      a.lastRep = now;
      sendKey(a.name, true);
    } else if (a.kind == A_SCROLL && now - a.downAt > 350 && now - a.lastRep >= 70) {
      a.lastRep = now;
      sendWheel(a.arg ? 1 : -1, 0);
    } else if (a.kind == A_MOVE) {
      if (a.arg == 0) vy -= 1; else if (a.arg == 1) vy += 1; else if (a.arg == 2) vx -= 1; else vx += 1;
      if (!moveSince || a.downAt < moveSince) moveSince = a.downAt;
    }
  }

  // Mouse movement in 1/8 view-pixel units; speed ramps up while held.
  float dt = lastMoveTick ? (now - lastMoveTick) / 1000.0f : 0;
  lastMoveTick = now;
  if (vx != 0 || vy != 0) {
    float held = (now - moveSince) / 1000.0f;
    float speed = (cur & kbit(IDX_SHIFT)) ? 18.0f : 70.0f + min(held / 0.8f, 1.0f) * 480.0f;  // view px/s
    if (vx && vy) speed *= 0.7071f;
    moveAccX += vx * speed * dt * 8;
    moveAccY += vy * speed * dt * 8;
    if (now - lastMoveSend >= 15) {
      int16_t dx = (int16_t)moveAccX, dy = (int16_t)moveAccY;
      if (dx || dy) { sendMove(dx, dy); moveAccX -= dx; moveAccY -= dy; lastMoveSend = now; }
    }
  } else {
    moveAccX = moveAccY = 0;
  }
}

// ============================================================================
// Session: incoming stream, read by rxTask() on core 0 so TCP keeps flowing
// while loop() on core 1 decodes and draws.
//   'T' tile | 'F' frame end | 'S' audio | 'H' heartbeat | 'X' bye
// ============================================================================
constexpr int TILE_SLOTS = 2;  // one being filled from the network, one being drawn
uint8_t* tileSlot[TILE_SLOTS];
struct RxItem { uint8_t type, slot; uint16_t x, y; uint32_t len; uint8_t hdr[11]; };
QueueHandle_t rxQueue = nullptr, freeSlots = nullptr;
volatile bool rxStop = false, rxRunning = false;
volatile uint32_t rxBytes = 0, lastRxAt = 0;
const char* volatile rxError = "";
uint32_t lastPingAt = 0;
uint8_t byeReason = 255;  // 255 = no goodbye received (connection just dropped)

static inline uint16_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// ---- Audio: 24 kHz mono mu-law -> ring buffer -> M5 speaker -------------------
constexpr uint32_t AUDIO_RATE = 24000;
constexpr uint32_t AUDIO_RING = 16384;            // power of two, ~680 ms
constexpr uint32_t AUDIO_CHUNK = 480;             // 20 ms per playRaw()
constexpr uint32_t AUDIO_PRIME = AUDIO_CHUNK * 6; // 120 ms jitter buffer before (re)starting
constexpr uint32_t AUDIO_LATE = AUDIO_CHUNK * 18; // > 360 ms queued: skip ahead (clock drift)
constexpr int AUDIO_BUFS = 4;                     // playing + queued + one being filled, +1 spare
constexpr uint8_t AUDIO_CH = 0;
uint8_t* audioRing = nullptr;
int16_t* audioPlay[AUDIO_BUFS];
int16_t ulawTable[256];
volatile uint32_t audioHead = 0, audioTail = 0;   // free-running; head = rxTask, tail = audioTask
volatile bool audioRun = false, audioAlive = false;

void buildUlawTable() {
  // Inverse of the server's continuous mu-law (mu = 255): q 0..255 -> pcm.
  for (int q = 0; q < 256; ++q) {
    float y = q / 255.0f * 2.0f - 1.0f;
    float x = (powf(256.0f, fabsf(y)) - 1.0f) / 255.0f;
    ulawTable[q] = (int16_t)((y < 0 ? -x : x) * 32000.0f);
  }
}

void audioTask(void*) {
  int k = 0;
  bool primed = false;
  while (audioRun) {
    uint32_t avail = audioHead - audioTail;
    if (avail > AUDIO_LATE) { audioTail = audioHead - AUDIO_PRIME; avail = AUDIO_PRIME; }
    if (!primed) primed = avail >= AUDIO_PRIME;
    else if (avail < AUDIO_CHUNK) primed = false;  // underrun: rebuild the jitter buffer
    if (primed && M5Cardputer.Speaker.isPlaying(AUDIO_CH) < 2) {
      int16_t* out = audioPlay[k];
      uint32_t t = audioTail;
      for (uint32_t i = 0; i < AUDIO_CHUNK; ++i) out[i] = ulawTable[audioRing[(t + i) & (AUDIO_RING - 1)]];
      audioTail = t + AUDIO_CHUNK;
      M5Cardputer.Speaker.playRaw(out, AUDIO_CHUNK, AUDIO_RATE, false, 1, AUDIO_CH, false);
      k = (k + 1) % AUDIO_BUFS;
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(3));
  }
  M5Cardputer.Speaker.stop(AUDIO_CH);
  audioAlive = false;
  vTaskDelete(nullptr);
}

void startAudio() {
  audioHead = audioTail = 0;
  M5Cardputer.Speaker.setVolume(volume);
  audioRun = audioAlive = true;
  xTaskCreatePinnedToCore(audioTask, "cxl_audio", 3072, nullptr, 4, nullptr, 0);
}
void stopAudio() {
  audioRun = false;
  while (audioAlive) delay(1);
}
void sendSoundWanted() { uint8_t p[1] = {(uint8_t)soundOn}; sendMsg('U', p, 1); }

// ---- Network task --------------------------------------------------------------
bool rxRead(uint8_t* dst, size_t n) {
  while (n) {
    if (rxStop) return false;
    int r = conn.read(dst, n);
    if (r > 0) { dst += r; n -= r; rxBytes += r; lastRxAt = millis(); continue; }
    if (!conn.connected()) { rxError = "Connection lost"; return false; }
    if (millis() - lastRxAt > 8000) { rxError = "Server stopped responding"; return false; }
    vTaskDelay(1);
  }
  return true;
}

bool rxPush(const RxItem& it) {
  while (xQueueSend(rxQueue, &it, pdMS_TO_TICKS(50)) != pdTRUE) if (rxStop) return false;
  return true;
}

void rxTask(void*) {
  RxItem it = {};
  uint8_t type, hdr[12], scratch[256];
  bool ok = true;
  while (ok && !rxStop) {
    if (!rxRead(&type, 1)) break;
    switch (type) {
      case 'T': {
        if (!(ok = rxRead(hdr, 12))) break;
        uint32_t len = rd32(hdr + 8);
        if (!len || len > JPEG_MAX) { rxError = "Tile too big"; ok = false; break; }
        uint8_t slot;
        while (xQueueReceive(freeSlots, &slot, pdMS_TO_TICKS(50)) != pdTRUE) if (rxStop) { ok = false; break; }
        if (!ok) break;
        if (!(ok = rxRead(tileSlot[slot], len))) { xQueueSend(freeSlots, &slot, 0); break; }
        it.type = 'T'; it.slot = slot; it.x = rd16(hdr); it.y = rd16(hdr + 2); it.len = len;
        ok = rxPush(it);
        break;
      }
      case 'F':
        if (!(ok = rxRead(it.hdr, 11))) break;
        it.type = 'F';
        ok = rxPush(it);
        break;
      case 'S': {
        if (!(ok = rxRead(hdr, 2))) break;
        uint32_t len = rd16(hdr);
        uint32_t space = AUDIO_RING - 1 - (audioHead - audioTail);
        if (audioRun && len <= space) {
          // Straight into the ring (in at most two pieces at the wrap point).
          uint32_t h = audioHead, first = min(len, AUDIO_RING - (h & (AUDIO_RING - 1)));
          if (!(ok = rxRead(audioRing + (h & (AUDIO_RING - 1)), first))) break;
          if (first < len && !(ok = rxRead(audioRing, len - first))) break;
          audioHead = h + len;
        } else {
          while (len && ok) { uint32_t n = min<uint32_t>(len, sizeof(scratch)); ok = rxRead(scratch, n); len -= n; }
        }
        break;
      }
      case 'M': {  // temperatures
        uint8_t cnt;
        if (!(ok = rxRead(&cnt, 1))) break;
        uint8_t stored = 0;
        for (uint8_t i = 0; i < cnt && ok; ++i) {
          uint8_t nl, name[32], tv[2];
          if (!(ok = rxRead(&nl, 1))) break;
          if (nl > sizeof(name)) { rxError = "Protocol error"; ok = false; break; }
          if (!(ok = rxRead(name, nl)) || !(ok = rxRead(tv, 2))) break;
          if (stored < MAX_TEMPS) {
            uint8_t k = min<uint8_t>(nl, 12);
            memcpy(tempNames[stored], name, k); tempNames[stored][k] = 0;
            tempX10[stored] = (int16_t)(tv[0] | (tv[1] << 8));
            ++stored;
          }
        }
        if (ok) { tempCount = stored; tempsAt = millis(); }
        break;
      }
      case 'H':
        break;
      case 'X':
        if (!(ok = rxRead(hdr, 1))) break;
        it.type = 'X'; it.hdr[0] = hdr[0];
        rxPush(it);
        ok = false;
        rxError = nullptr;
        break;
      default:
        rxError = "Protocol error";
        ok = false;
    }
  }
  if (!rxStop && rxError) { it.type = 'E'; rxPush(it); }
  rxRunning = false;
  vTaskDelete(nullptr);
}

// ---- Drawing: JPEGDEC hands over MCU blocks in a ping-pong buffer, so the SPI
// DMA of one block overlaps with decoding the next.
int jpegDraw(JPEGDRAW* d) {
  tft.pushImageDMA(d->x, d->y, d->iWidth, d->iHeight, (const lgfx::swap565_t*)d->pPixels);
  return 1;
}

void drawTile(const RxItem& it) {
  if (!jpeg.openRAM(tileSlot[it.slot], it.len, jpegDraw)) return;
  jpeg.setPixelType(RGB565_BIG_ENDIAN);
  tft.startWrite();
  jpeg.decode(it.x, it.y, JPEG_USES_DMA);
  tft.endWrite();  // waits for the last DMA block
  jpeg.close();
}

// Returns false when the session is over.
bool serviceRx() {
  RxItem it;
  uint32_t t0 = millis();
  while (millis() - t0 < 12 && xQueueReceive(rxQueue, &it, 0) == pdTRUE) {
    switch (it.type) {
      case 'T':
        drawTile(it);
        xQueueSend(freeSlots, &it.slot, 0);
        break;
      case 'F': {
        sendMsg('A', it.hdr, 4);  // ack only after every tile of the frame is on screen
        uint16_t ratio = rd16(it.hdr + 4);
        bool stretch = it.hdr[10] & 1, audio = it.hdr[10] & 2, cursor = !(it.hdr[10] & 4);
        if (ratio != viewRatioX100 || it.hdr[6] != zoomIdx || it.hdr[9] != monitorNo || stretch != stretchOn || audio != audioStreaming || cursor != cursorShown) hudDirty = true;
        stretchOn = stretch; audioStreaming = audio; cursorShown = cursor;
        viewRatioX100 = ratio; zoomIdx = it.hdr[6]; zoomCount = it.hdr[7]; monitorNo = it.hdr[9];
        if (it.hdr[8] != quality && it.hdr[8] < 3) { quality = it.hdr[8]; hudDirty = true; }
        ++statFrames;
        break;
      }
      case 'X':
        byeReason = it.hdr[0];
        connError = byeReason == 2 ? "Another client took over" : (byeReason == 0 ? "Disconnected by the PC" : "Server closed session");
        return false;
      case 'E':
        connError = rxError ? rxError : "Connection lost";
        return false;
    }
  }
  return true;
}

void runSession() {
  inSession = true; exitRequested = false; byeReason = 255;
  mouseMode = false; audioStreaming = false;
  sessPrevMask = readKeyMask();  // keys still held from the menu are ignored
  for (auto& a : acts) a.kind = A_NONE;
  optUsed = optWinDown = false;
  lastRxAt = lastPingAt = statT0 = millis();
  statFrames = 0; rxBytes = 0; fps = kbps = 0;
  lastKeyLabel = "";
  tft.fillScreen(C_BG);

  xQueueReset(rxQueue);
  xQueueReset(freeSlots);
  for (uint8_t i = 0; i < TILE_SLOTS; ++i) xQueueSend(freeSlots, &i, 0);
  rxStop = false; rxRunning = true; rxError = "Connection lost";
  xTaskCreatePinnedToCore(rxTask, "cxl_rx", 4096, nullptr, 5, nullptr, 0);
  startAudio();

  sendQuality();
  sendFpsCap();
  { uint8_t want = 1; sendMsg('G', &want, 1); }  // temperatures for the Opt+5 gauges
  sendSoundWanted();
  hudDirty = true;
  haptic(40);
  while (true) {
    if (!serviceRx()) break;
    serviceInput();
    if (exitRequested) { connError = ""; break; }
    uint32_t now = millis();
    if (now - lastPingAt > 2000) { sendMsg('P', nullptr, 0); lastPingAt = now; }
    if (now - statT0 >= 1000) {
      fps = statFrames * 1000.0f / (now - statT0);
      kbps = rxBytes / 1.024f / (now - statT0);
      statFrames = 0; rxBytes = 0; statT0 = now;
      hudDirty = true;
    }
    serviceHud();
    serviceHaptic();
    if (uxQueueMessagesWaiting(rxQueue) == 0) delay(1);
  }
  rxStop = true;
  while (rxRunning) delay(1);
  stopAudio();
  if (conn.connected()) releaseAll();
  conn.stop();
  inSession = false;
  hudDirty = true;
}

// ============================================================================
// Top-level flow: Wi-Fi -> pick server -> session -> back to picker
// ============================================================================
bool connectTo(IPAddress ip, uint16_t port) {
  while (true) {
    uiMessage("CONNECTING", ip.toString() + ":" + String(port));
    ConnResult r = remoteConnect(ip, port);
    if (r == CONN_OK) {
      lastServerIp = ip; lastServerPort = port; saveSettings();
      return true;
    }
    if (r == CONN_BADPIN) {
      String pin = serverPin;
      String label = serverPin.isEmpty() ? String("Enter the PIN shown by the server") : String("Wrong PIN - try again");
      if (!textInput("SERVER PIN", label, pin, false, 16)) return false;
      serverPin = pin; saveSettings();
      continue;
    }
    uiMessage("CONNECT FAILED", connError, "Is cxl_remote_server.py running and allowed by the firewall?", C_ERR, "any key: back");
    waitAnyKey(6000);
    return false;
  }
}

// Server picker. Returns true when a session got connected.
bool pickServer() {
  servers.clear();
  int sel = 0, top = 0;
  uint32_t lastBroadcast = 0;
  bool dirty = true;
  uiHeader("REMOTE DESKTOP");
  uiFooter("ENTER connect  M manual IP  P PIN  W Wi-Fi  H hotspot  ` rescan");
  setStatus("Pick a PC", "Searching the network...");
  while (true) {
    uint32_t now = millis();
    if (now - lastBroadcast > 1000) { discoveryBroadcast(); lastBroadcast = now; }
    if (discoveryPoll()) dirty = true;
    std::vector<String> items;
    for (auto& s : servers) items.push_back(s.name.substring(0, 13) + " " + s.ip.toString());
    items.push_back("Manual IP...");
    items.push_back("Server PIN...");
    items.push_back("Wi-Fi setup...");
    items.push_back(String("Hotspot for PC: ") + (hotspotOn ? "ON" : "OFF"));
    if (hotspotOn) items.push_back("Hotspot name/password...");
    if (sel >= (int)items.size()) sel = items.size() - 1;
    if (dirty) {
      uiDrawList(items, sel, top);
      setStatus("Pick a PC", servers.empty() ? String("Searching the network...") : String(servers.size()) + " server(s) found");
      dirty = false;
    }
    serviceHud(); serviceHaptic();
    UiKey k = uiPoll();
    if (k.idx < 0) { delay(10); continue; }
    int action = -1;  // 0..n-1 server, n manual, n+1 pin, n+2 wifi, n+3 hotspot, n+4 hotspot creds
    int n = servers.size();
    if (k.base == ';' && sel > 0) { --sel; dirty = true; }
    else if (k.base == '.' && sel + 1 < (int)items.size()) { ++sel; dirty = true; }
    else if (k.base == KEY_ENTER) action = sel;
    else if (k.base == 'm') action = n;
    else if (k.base == 'p') action = n + 1;
    else if (k.base == 'w') action = n + 2;
    else if (k.base == 'h') action = n + 3;
    else if (k.base == '`') { servers.clear(); sel = 0; dirty = true; }
    if (action < 0) continue;
    if (action < n) {
      if (connectTo(servers[action].ip, servers[action].port)) return true;
    } else if (action == n) {
      String ipText = lastServerIp != IPAddress((uint32_t)0) ? lastServerIp.toString() : String("192.168.");
      if (textInput("MANUAL IP", "PC address (port " + String(REMOTE_PORT) + ")", ipText, false, 15)) {
        IPAddress ip;
        if (ip.fromString(ipText) && connectTo(ip, REMOTE_PORT)) return true;
      }
    } else if (action == n + 1) {
      String pin = serverPin;
      if (textInput("SERVER PIN", "PIN printed by cxl_remote_server.py", pin, false, 16)) { serverPin = pin; saveSettings(); }
    } else if (action == n + 2) {
      wifiSetup();
      while (!connectWifi()) wifiSetup();
    } else if (action == n + 3) {
      hotspotOn = !hotspotOn;
      if (hotspotOn) {
        startHotspot();
        uiMessage("HOTSPOT ON", apSsid, "Password: " + apPass + "   PC/UNO Q joins this network.", C_OK, "any key: back");
        waitAnyKey(15000);
      } else {
        stopHotspot();
      }
      saveSettings();
      servers.clear();
    } else if (hotspotOn) {
      String ssid = apSsid, pass = apPass;
      if (textInput("HOTSPOT NAME", "Network name for the PC to join", ssid, false, 31) && ssid.length() &&
          textInput("HOTSPOT PASSWORD", "At least 8 characters", pass, false, 63) && pass.length() >= 8) {
        apSsid = ssid; apPass = pass; saveSettings();
        startHotspot();
      }
    }
    uiHeader("REMOTE DESKTOP");
    uiFooter("ENTER connect  M manual IP  P PIN  W Wi-Fi  H hotspot  ` rescan");
    dirty = true;
  }
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  loadSettings();
  pinMode(BTNA_PIN, INPUT_PULLUP);
  pinMode(HAPTIC_PIN, OUTPUT);
  digitalWrite(HAPTIC_PIN, LOW);

  tft.init();
  // CardputerXL's saved rotation is in Adafruit_ILI9341 terms; with this init
  // sequence lgfx lands 180 degrees off, so flip it (3 <-> 1).
  tft.setRotation((rotation + 2) & 3);
  tft.setTextWrap(false);
  tft.fillScreen(C_BG);
  ledcAttach(TFT_BL, BL_HZ, BL_BITS);
  applyBrightness();

  hud.setColorDepth(8);
  hud.createSprite(240, 135);

  bool memOk = true;
  for (int i = 0; i < TILE_SLOTS; ++i) memOk &= (tileSlot[i] = (uint8_t*)malloc(JPEG_MAX)) != nullptr;
  memOk &= (audioRing = (uint8_t*)malloc(AUDIO_RING)) != nullptr;
  for (int i = 0; i < AUDIO_BUFS; ++i) memOk &= (audioPlay[i] = (int16_t*)malloc(AUDIO_CHUNK * 2)) != nullptr;
  rxQueue = xQueueCreate(8, sizeof(RxItem));
  freeSlots = xQueueCreate(TILE_SLOTS, 1);
  buildUlawTable();
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(volume);

  for (int i = 0; i < KEY_COUNT; ++i) acts[i].kind = A_NONE;

  uiMessage("CXL REMOTE", "Remote desktop", "for the Cardputer XL  -  v" CXL_REMOTE_VERSION);
  if (!memOk) { uiMessage("ERROR", "Out of memory", "", C_ERR); while (true) delay(1000); }
  delay(400);
  while (!connectWifi()) wifiSetup();
}

void loop() {
  // Reconnect straight to the last PC first; fall back to the picker.
  static bool triedLast = false;
  bool connected = false;
  if (!triedLast && lastServerIp != IPAddress((uint32_t)0)) {
    triedLast = true;
    uiMessage("CONNECTING", lastServerIp.toString(), "last used PC");
    connected = remoteConnect(lastServerIp, lastServerPort) == CONN_OK;
  }
  if (!connected) connected = pickServer();
  if (!connected) return;

  runSession();
  if (connError.length()) {
    // Unexpected drop: retry the same PC a few times before giving up.
    String why = connError;
    for (int attempt = 1; attempt <= 3 && byeReason == 255; ++attempt) {
      if (WiFi.status() != WL_CONNECTED && !hotspotOn) while (!connectWifi()) wifiSetup();
      uiMessage("RECONNECTING", why, "attempt " + String(attempt) + "/3", C_ERR);
      delay(800);
      if (remoteConnect(lastServerIp, lastServerPort) == CONN_OK) { runSession(); if (!connError.length()) return; why = connError; attempt = 0; }
    }
    uiMessage("DISCONNECTED", why, "", C_ERR, "any key: back to the server list");
    waitAnyKey(5000);
  }
}
