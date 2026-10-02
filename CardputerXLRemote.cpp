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
// the mouse). The Cardputer only receives tiles, decodes them with
// TJpg_Decoder straight onto the panel, and forwards the keyboard.
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
//   MOUSE mode: ; . , / move (accelerates, Shift = slow), Enter/Space = left
//   button (hold to drag), \ = right, ' = middle, [ ] = wheel. Every other
//   key still types.
//
// BUILD: FQBN esp32:esp32:m5stack_cardputer, libraries M5Cardputer,
// Adafruit ILI9341 + GFX, TJpg_Decoder.
// ============================================================================

#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <TJpg_Decoder.h>
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
constexpr uint8_t PROTO_VER = 2;
#define CXL_REMOTE_VERSION "1.0.0"
constexpr int OUT_W = 320, OUT_H = 240;
constexpr uint32_t JPEG_MAX = 48 * 1024;     // server splits tiles to fit
constexpr uint32_t BL_HZ = 5000;
constexpr uint8_t BL_BITS = 8;

constexpr uint16_t C_BG = ILI9341_BLACK;
constexpr uint16_t C_BAR = 0x0233;
constexpr uint16_t C_ACC = 0x2E7F;
constexpr uint16_t C_TXT = ILI9341_WHITE;
constexpr uint16_t C_DIM = 0x8C71;
constexpr uint16_t C_SEL = 0x1A6B;
constexpr uint16_t C_ERR = 0xFA48;
constexpr uint16_t C_OK = 0x47E8;

Adafruit_ILI9341 tft(&SPI, TFT_DC, TFT_CS, TFT_RST);
M5Canvas hud(&M5Cardputer.Display);
Preferences prefs;
WiFiClient conn;
WiFiUDP udp;
uint8_t* jpegBuf = nullptr;

// ---- Persistent settings ----
String wifiSsid, wifiPass, serverPin;
IPAddress lastServerIp;
uint16_t lastServerPort = REMOTE_PORT;
uint8_t rotation = 3, brightness = 10, quality = 1;
const char* qualityNames[] = {"FAST", "NORMAL", "SHARP"};

// ---- Session / HUD state ----
String serverName, statusLine = "Starting", statusLine2;
uint16_t serverScreenW = 0, serverScreenH = 0;
bool mouseMode = false, inSession = false, hudDirty = true;
uint16_t viewRatioX100 = 0;
uint8_t zoomIdx = 0, zoomCount = 1, monitorNo = 1;
bool stretchOn = false;
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
    hud.setTextColor((uint16_t)0x8C71);
    hud.setCursor(4, 112); hud.print("Use the big screen. ` = back");
    hud.setCursor(4, 124); hud.print("PC: python cxl_remote_server.py");
    hud.pushSprite(0, 0);
    return;
  }

  hud.setTextColor((uint16_t)TFT_LIGHTGREY);
  hud.setCursor(4, 19); hud.print((serverName + "  " + String(serverScreenW) + "x" + String(serverScreenH)).substring(0, 39));

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
  hud.printf("%4.1f fps  %4.0f KB/s  Q:%s", fps, kbps, qualityNames[quality]);
  hud.setCursor(4, 72); hud.setTextColor((uint16_t)0x8C71);
  hud.print(("key: " + lastKeyLabel).substring(0, 39));

  hud.setTextColor((uint16_t)0x2E7F);
  const char* const* lines;
  static const char* keyHints[] = {"Fn+;.,/ arrows  Fn+` Esc  Fn+1-= F1-12",
                                   "Fn+Del Delete  Fn+[] PgUp/Dn  Fn+H/E",
                                   "Opt=Win Opt+Spc/G0 mouse Opt+-= zoom",
                                   "Opt+0 fit/stretch Opt+9 qual Opt+` quit"};
  static const char* mouseHints[] = {";.,/ move (Shift slow)  Ent/Spc left",
                                     "\\ right  ' middle  [ ] wheel  hold=drag",
                                     "Opt+Spc/G0 keybd  Opt+-= zoom  Opt+0 fit",
                                     "other keys still type  Opt+` quit"};
  lines = mouseMode ? mouseHints : keyHints;
  for (int i = 0; i < 4; ++i) { hud.setCursor(2, 87 + i * 12); hud.print(lines[i]); }
  hud.pushSprite(0, 0);
}

void serviceHud(bool force = false) {
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
bool connectWifi() {
  if (wifiSsid.isEmpty()) return false;
  uiMessage("WI-FI", "Connecting to", "", C_DIM, "` cancel -> Wi-Fi setup");
  uiText(10, 76, wifiSsid.substring(0, 25), C_ACC);
  setStatus("Wi-Fi...", wifiSsid);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // modem sleep adds ~100 ms of input latency
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uint32_t t0 = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 20000) return false;
    UiKey k = uiPoll();
    if (k.base == '`') return false;
    if ((millis() - t0) / 400 != (uint32_t)dots) {
      dots = (millis() - t0) / 400;
      tft.fillRect(10, 110, 300, 8, C_BG);
      for (int i = 0; i < dots % 25; ++i) tft.fillRect(10 + i * 12, 110, 8, 8, C_ACC);
    }
    serviceHud();
    delay(20);
  }
  udp.begin(DISCOVERY_LOCAL_PORT);
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
  udp.beginPacket(WiFi.broadcastIP(), REMOTE_PORT); udp.write(q, sizeof(q)); udp.endPacket();
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
    case KEY_ENTER: sendMsg('R', nullptr, 0); haptic(); return true;
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
// Session: incoming stream  'T' tile | 'F' frame end | 'H' heartbeat | 'X' bye
// ============================================================================
enum RxState { RX_TYPE, RX_HDR, RX_BODY };
RxState rxState = RX_TYPE;
uint8_t rxType = 0, rxHdr[16];
int rxHdrNeed = 0, rxHdrGot = 0;
uint32_t rxBodyLen = 0, rxBodyGot = 0, lastRxAt = 0, lastPingAt = 0;
uint16_t tileX, tileY, tileW, tileH;
uint8_t byeReason = 255;  // 255 = no goodbye received (connection just dropped)

bool jpgOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (x < 0 || y < 0 || x + w > OUT_W || y + h > OUT_H) return true;
  tft.setAddrWindow(x, y, w, h);
  tft.writePixels(bmp, (uint32_t)w * h);
  return true;
}

int headerSize(uint8_t t) {
  switch (t) { case 'T': return 12; case 'F': return 11; case 'H': return 0; case 'X': return 1; }
  return -1;
}

static inline uint16_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// Returns false on protocol error / server goodbye.
bool handleHeader() {
  switch (rxType) {
    case 'T':
      tileX = rd16(rxHdr); tileY = rd16(rxHdr + 2); tileW = rd16(rxHdr + 4); tileH = rd16(rxHdr + 6);
      rxBodyLen = rd32(rxHdr + 8);
      if (rxBodyLen == 0 || rxBodyLen > JPEG_MAX) { connError = "Tile too big"; return false; }
      rxBodyGot = 0;
      rxState = RX_BODY;
      return true;
    case 'F': {
      uint8_t ack[4]; memcpy(ack, rxHdr, 4);
      sendMsg('A', ack, 4);
      uint16_t ratio = rd16(rxHdr + 4);
      bool stretch = rxHdr[10] & 1;
      if (ratio != viewRatioX100 || rxHdr[6] != zoomIdx || rxHdr[9] != monitorNo || stretch != stretchOn) hudDirty = true;
      stretchOn = stretch;
      viewRatioX100 = ratio; zoomIdx = rxHdr[6]; zoomCount = rxHdr[7]; monitorNo = rxHdr[9];
      if (rxHdr[8] != quality && rxHdr[8] < 3) { quality = rxHdr[8]; hudDirty = true; }
      ++statFrames;
      break;
    }
    case 'X': byeReason = rxHdr[0]; connError = byeReason == 2 ? "Another client took over" : (byeReason == 0 ? "Disconnected by the PC" : "Server closed session"); return false;
  }
  rxState = RX_TYPE;
  return true;
}

bool serviceRx() {
  uint32_t t0 = millis();
  while (millis() - t0 < 8) {
    int avail = conn.available();
    if (avail <= 0) break;
    lastRxAt = millis();
    if (rxState == RX_TYPE) {
      rxType = conn.read();
      rxHdrNeed = headerSize(rxType);
      rxHdrGot = 0;
      statBytes += 1;
      if (rxHdrNeed < 0) { connError = "Protocol error"; return false; }
      if (rxHdrNeed == 0) { if (!handleHeader()) return false; }
      else rxState = RX_HDR;
    } else if (rxState == RX_HDR) {
      int r = conn.read(rxHdr + rxHdrGot, min(avail, rxHdrNeed - rxHdrGot));
      if (r <= 0) break;
      rxHdrGot += r; statBytes += r;
      if (rxHdrGot == rxHdrNeed && !handleHeader()) return false;
    } else {
      int r = conn.read(jpegBuf + rxBodyGot, min<uint32_t>(avail, rxBodyLen - rxBodyGot));
      if (r <= 0) break;
      rxBodyGot += r; statBytes += r;
      if (rxBodyGot == rxBodyLen) {
        tft.startWrite();
        TJpgDec.drawJpg(tileX, tileY, jpegBuf, rxBodyLen);
        tft.endWrite();
        rxState = RX_TYPE;
        break;  // give the keyboard a turn after every decoded tile
      }
    }
  }
  return true;
}

void runSession() {
  inSession = true; exitRequested = false; byeReason = 255;
  mouseMode = false; rxState = RX_TYPE;
  sessPrevMask = readKeyMask();  // keys still held from the menu are ignored
  for (auto& a : acts) a.kind = A_NONE;
  optUsed = optWinDown = false;
  lastRxAt = lastPingAt = statT0 = millis();
  statFrames = statBytes = 0; fps = kbps = 0;
  lastKeyLabel = "";
  tft.fillScreen(ILI9341_BLACK);
  sendQuality();
  hudDirty = true;
  haptic(40);
  while (true) {
    if (!conn.connected() && conn.available() <= 0) { connError = "Connection lost"; break; }
    if (!serviceRx()) break;
    serviceInput();
    if (exitRequested) { connError = ""; break; }
    uint32_t now = millis();
    if (now - lastPingAt > 2000) { sendMsg('P', nullptr, 0); lastPingAt = now; }
    if (now - lastRxAt > 8000) { connError = "Server stopped responding"; break; }
    if (now - statT0 >= 1000) {
      fps = statFrames * 1000.0f / (now - statT0);
      kbps = statBytes / 1.024f / (now - statT0);
      statFrames = statBytes = 0; statT0 = now;
      hudDirty = true;
    }
    serviceHud();
    serviceHaptic();
  }
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
  uiFooter("ENTER connect  M manual IP  P PIN  W Wi-Fi  ` rescan");
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
    if (sel >= (int)items.size()) sel = items.size() - 1;
    if (dirty) {
      uiDrawList(items, sel, top);
      setStatus("Pick a PC", servers.empty() ? String("Searching the network...") : String(servers.size()) + " server(s) found");
      dirty = false;
    }
    serviceHud(); serviceHaptic();
    UiKey k = uiPoll();
    if (k.idx < 0) { delay(10); continue; }
    int action = -1;  // 0..n-1 server, n manual, n+1 pin, n+2 wifi
    int n = servers.size();
    if (k.base == ';' && sel > 0) { --sel; dirty = true; }
    else if (k.base == '.' && sel + 1 < (int)items.size()) { ++sel; dirty = true; }
    else if (k.base == KEY_ENTER) action = sel;
    else if (k.base == 'm') action = n;
    else if (k.base == 'p') action = n + 1;
    else if (k.base == 'w') action = n + 2;
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
    } else {
      wifiSetup();
      while (!connectWifi()) wifiSetup();
    }
    uiHeader("REMOTE DESKTOP");
    uiFooter("ENTER connect  M manual IP  P PIN  W Wi-Fi  ` rescan");
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

  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(rotation);
  tft.setTextWrap(false);
  tft.fillScreen(C_BG);
  ledcAttach(TFT_BL, BL_HZ, BL_BITS);
  applyBrightness();

  hud.setColorDepth(8);
  hud.createSprite(240, 135);

  jpegBuf = (uint8_t*)malloc(JPEG_MAX);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);  // writePixels() already sends big-endian
  TJpgDec.setCallback(jpgOut);

  for (int i = 0; i < KEY_COUNT; ++i) acts[i].kind = A_NONE;

  uiMessage("CXL REMOTE", "Remote desktop", "for the Cardputer XL  -  v" CXL_REMOTE_VERSION);
  if (!jpegBuf) { uiMessage("ERROR", "Out of memory", "", C_ERR); while (true) delay(1000); }
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
      if (WiFi.status() != WL_CONNECTED) while (!connectWifi()) wifiSetup();
      uiMessage("RECONNECTING", why, "attempt " + String(attempt) + "/3", C_ERR);
      delay(800);
      if (remoteConnect(lastServerIp, lastServerPort) == CONN_OK) { runSession(); if (!connError.length()) return; why = connError; attempt = 0; }
    }
    uiMessage("DISCONNECTED", why, "", C_ERR, "any key: back to the server list");
    waitAnyKey(5000);
  }
}
