# CXL Remote

Remote desktop for the **Cardputer XL**: an [M5Stack Cardputer-ADV](https://docs.m5stack.com/en/core/Cardputer) with an external **ILI9341 320x240** panel on the EXT header (same wiring as [cardputer-xl](https://github.com/szuszjan/cardputer-xl)).

A PC's screen goes to the big panel and **its sound plays on the Cardputer's speaker**. The Cardputer's keyboard drives the PC: typing, shortcuts, mouse, zoom and multiple monitors all work.

The PC can be:
- **a Windows PC**, with a ready-to-run `CXLRemoteServer.exe`;
- **an [Arduino UNO Q](https://docs.arduino.cc/hardware/uno-q/) as a pocket mini PC.** It runs headless on a virtual 640x480 screen (exactly 2x the panel) with the Cardputer as its only screen, keyboard and speaker. It can also show a PIN, a clock and a sound spectrum on its own LED matrix.

The two screens:
- **External ILI9341**: the PC screen.
- **Built-in Cardputer screen**: status HUD with KEYBOARD/MOUSE mode, zoom, fps and cap, KB/s, sound, the PC's CPU temperature, and key hints or every temperature sensor.

**How the work is split.** The PC captures the screen, scales it to 320x240, overlays the cursor, finds the regions that changed and JPEG-encodes them. On the Cardputer:
- core 0 receives the stream;
- core 1 decodes tiles with JPEGDEC while the previous block is already going to the panel over SPI DMA;
- an audio task plays the sound from a jitter buffer.

While a lot of the screen moves (video, scrolling), tiles go out at a lower quality and are re-sent sharp once the picture holds still.

## Quick start (Windows PC)

1. **PC**: download `CXLRemoteServer.exe` from [Releases](../../releases) and run it. It needs no install and no Python.
   - It is unsigned, so SmartScreen may warn: *More info → Run anyway*.
   - When the firewall asks, allow **private networks only**.
2. **Cardputer**: flash `cxl-remote-firmware-merged.bin` at `0x0` (M5Burner, or `esptool.py write_flash 0x0 cxl-remote-firmware-merged.bin`).
3. On the Cardputer:
   - pick a Wi-Fi network;
   - pick the PC from the list (found automatically);
   - enter the **PIN** shown by the server, once.

The Cardputer remembers the PC and reconnects to it on the next boot.

## Arduino UNO Q as a mini PC

The UNO Q runs Debian 13 with XFCE. `uno_q/setup.sh` turns it into a headless desktop for the Cardputer:
- Xorg dummy driver: a virtual 640x480 monitor, 4:3 like the panel, so FIT has no bars.
- XFCE autologin, with the server started with the desktop.
- No screen blanking or locking.
- A virtual audio output (PipeWire null sink) whose sound goes to the Cardputer.
- USB keyboards and mice plugged into the UNO Q (through a USB-C hub) keep working alongside.

Setup, from a PC on the same network (the board must have finished its first App Lab setup, user `arduino`):

```bash
scp -r cxl-remote-uno-q arduino@<board>.local:~/cxlremote
```

```bash
ssh -t arduino@<board>.local "sudo bash ~/cxlremote/setup.sh && sudo reboot"
```

After the reboot:
- **PIN**: `cat ~/.cxl_remote.json` on the board, or read it off the LED matrix (see below).
- **Clock**: the matrix clock uses the system time zone, e.g. `sudo timedatectl set-timezone Europe/Warsaw`.
- **Other screen size**: `sudo bash setup.sh 800x600`.
- **Real monitor**: while `/etc/X11/xorg.conf.d/90-cxlremote-dummy.conf` exists, a real monitor stays dark. Delete that file to use one.

Light browsers worth installing on the 2 GB board: `epiphany-browser` (full modern web) and `netsurf-gtk` (instant, simple pages). Luanti (Minetest) runs too, software-rendered (the virtual screen has no GPU): copy `uno_q/luanti-low.conf` to `~/.minetest/minetest.conf` for settings that hold ~20 fps, and use a 20–30 fps cap (`Opt+6`), which is smoother than MAX while a game is running.

### LED matrix

Flash `uno_q/matrix_mcu/matrix_mcu.ino` to the UNO Q's microcontroller once.
- **How:** Arduino IDE or App Lab, FQBN `arduino:zephyr:unoq`, library `Arduino_RouterBridge`; upload over USB.
- **Design:** the sketch only displays 13x8 frames. The server composes them and sends them through the board's `arduino-router`.

What the matrix shows:

| State | Matrix |
|---|---|
| no Cardputer connected (also while its server list is open) | the **PIN** scrolls |
| Cardputer connecting | ripples |
| connected / wrong PIN | a tick / a blinking X |
| connected | scrolling **clock** |
| sound playing | 13-band **spectrum** (back to the clock after 2 s of silence) |
| disconnected | the picture crumbles away |

### Hotspot mode (Cardputer as the UNO Q's Wi-Fi)

In the Cardputer's server list, press **H** ("Hotspot for PC").
- **What it does:** the Cardputer stays on the main Wi-Fi and also opens its own network `CXL-Remote-XXXX` (192.168.77.1/24). That network's traffic goes out through the main Wi-Fi with NAT.
- **Why:** the UNO Q gets a direct link to the Cardputer and still has internet.
- **Without a main network:** it works as an offline link.

Point the UNO Q at it once:

```bash
bash ~/cxlremote/join_hotspot.sh "CXL-Remote-XXXX" "<password shown on the Cardputer>" now
```

The home Wi-Fi stays saved as the fallback. `hotspot_watch.sh`, started with the desktop, moves the board back onto the hotspot whenever it reappears. While the UNO Q is behind the Cardputer's NAT, other machines can't reach it (no SSH from your PC); turn the hotspot off to get it back on the home network.

## Server window (Windows)

![icon](server/icon.png)

- Live preview of exactly what the Cardputer panel shows
- PIN: copy, new, hide, or turn off (trusted networks only)
- Live status: fps, KB/s, zoom, monitor, sound, connection time
- **Monitor**, **Keep aspect / Stretch to fit**, **quality** (Fast / Normal / Sharp), **max FPS** (used when the Cardputer's cap is SRV)
- **Send PC sound**, **Show the mouse cursor**, **View only** (ignore the Cardputer's keyboard and mouse)
- Disconnect the Cardputer, start/stop the server, log

Settings sync both ways and are stored in `~/.cxl_remote.json`. Windows doesn't let injected input reach **elevated (admin) windows** unless the server also runs as administrator.

## Keys

| Keys | Action |
|---|---|
| typing | real key down/up events with Shift/Ctrl/Alt as real modifiers: Ctrl+C, Ctrl+Shift+T, Alt+Tab… |
| Ctrl+Alt+letter | AltGr characters on Windows (e.g. Polish ą ę ś ł…) |
| Fn + `;` `.` `,` `/` | arrows |
| Fn + `` ` `` | Esc |
| Fn + `1`…`0` `-` `=` | F1…F12 |
| Fn + Del | Delete |
| Fn + `[` `]` | Page Up / Page Down |
| Fn + H / E | Home / End |
| Fn + I / P / M | Insert / PrintScreen / context Menu |
| Opt (tap) | Windows / Super key |
| Opt + key | Win + key (Win+R, Win+D, Win+E…) |
| **Opt + Space** or **G0** button | toggle MOUSE / KEYBOARD mode |
| Opt + `=` / `-` | zoom in / out (the view follows the pointer) |
| Opt + `0` | whole screen; press again to toggle **FIT ↔ STRETCH** |
| Opt + `1` | next monitor |
| Opt + `5` | small screen: key hints ↔ **temperature gauges** |
| Opt + `6` | frame-rate cap: MAX / 30 / 20 / 15 / SRV |
| Opt + `7` | show / hide the mouse cursor |
| Opt + `8` | backlight |
| Opt + `9` | image quality |
| Opt + `[` / `]` | volume down / up |
| Opt + `\` | sound on / off |
| Opt + Enter | full refresh |
| Opt + `` ` `` | disconnect (back to the server list) |

**MOUSE mode:**
- `;` `.` `,` `/` move the pointer (it accelerates; hold Shift for precision).
- Enter or Space is the left button; hold it to drag.
- `\` is the right button, `'` the middle button, `[` `]` the scroll wheel.
- Every other key still types.

**Server list:** ENTER connect, **M** manual IP, **P** PIN, **W** Wi-Fi setup, **H** hotspot, `` ` `` rescan.

**Zoom:**
- FIT is the whole monitor with its aspect kept.
- STRETCH fills all 320x240.
- After that come 1:4, 1:3, 1:2, 1:1.5 and 1:1 (pixel for pixel), as far as the monitor size allows.

**Frame-rate cap:**
- MAX lets the frame acknowledgements pace the stream.
- A fixed cap (e.g. 30) often looks smoother when the PC is busy (games), because it leaves CPU for the app.

**Temperatures:** the hottest CPU zone is always shown next to the resolution. `Opt+5` shows every thermal zone the server can read (Linux; Windows exposes none) as gauges from 30 to 95 °C.

## Building from source

**Firmware** (`CardputerXLRemote.cpp`; the `.ino` is only a stub):
- Arduino-ESP32 3.x, FQBN `esp32:esp32:m5stack_cardputer`.
- Libraries: M5Cardputer (its M5GFX also drives the external panel, on SPI2 with DMA) and JPEGDEC.

```bash
arduino-cli compile --upload -p COM25 --fqbn esp32:esp32:m5stack_cardputer .
```

If CardputerXL already ran on the unit, the sketch reuses its saved Wi-Fi network, rotation and brightness from NVS. Flashing only the app keeps NVS; the merged `.bin` erases it.

**Server** (Python 3.9+):

```bash
pip install -r server/requirements.txt
```

```bash
python server/cxl_remote_gui.pyw
```

Console only: `python server/cxl_remote_server.py` (`--port --pin --fps --stretch --no-auth -v`).

Platform support:
- **Windows:** input via SendInput, sound via WASAPI loopback.
- **Linux/X11:** input via XTEST (python3-xlib), sound via PulseAudio/PipeWire.

Rebuild the exe with `pip install pyinstaller`, then `server/build_exe.bat`.

## Wiring

| ILI9341 | Cardputer-ADV |
|---|---|
| CS | GPIO 5 |
| DC | GPIO 13 |
| RST | GPIO 15 |
| MOSI | GPIO 3 |
| SCK | GPIO 6 |
| LED / backlight | GPIO 39 (PWM) |
| optional vibration motor (IN) | GPIO 1 |

## Protocol & security

Protocol v3 is documented at the top of `server/cxl_remote_server.py`.
- Discovery is a UDP broadcast on port 5901; the session runs over TCP 5901.
- Screen updates are JPEG tiles on a 32x16 grid, merged into rectangles. At most 2 frames are in flight, each acknowledged.
- Sound uses the same connection: 20 ms blocks of 24 kHz mono mu-law.
- Temperatures are opt-in messages, so older firmware never sees them.
- The Cardputer authenticates with an HMAC-SHA256 challenge keyed by the PIN, so the PIN never crosses the network.
- The stream itself is **not encrypted**. Use it on your home network (or the Cardputer's own hotspot) only.
