#!/usr/bin/env python3
"""CXL Remote server - streams this PC's screen to a Cardputer XL and
injects its keyboard/mouse input.

    pip install -r requirements.txt
    python cxl_remote_gui.pyw              # window with preview, PIN and settings
    python cxl_remote_server.py            # same server, console only

The Cardputer finds this server by UDP broadcast on port 5901 and connects
over TCP 5901. Allow Python through the firewall on *private* networks only:
anyone who knows the PIN can control this PC. Input into elevated (admin)
windows is blocked by Windows unless this script runs as administrator.

Protocol v3 (little-endian):
  S->C  "CXLS" ver nonce[16]
  C->S  "CXLR" ver w:u16 h:u16 maxjpeg:u32 hmac_sha256(pin, nonce)[32]
  S->C  'O' screen_w:u16 screen_h:u16 namelen:u8 name   |  'X' reason:u8 (1 = bad PIN)
  then
  S->C  'T' x y w h:u16 len:u32 jpeg      tile
        'F' id:u32 ratio_x100:u16 zoom_idx zoom_count quality monitor flags (u8 each)
            flags bit0 = stretch-to-fit active, bit1 = sound streaming, bit2 = cursor hidden
        'S' len:u16 audio                 24 kHz mono, continuous mu-law (mu=255) bytes
        'H'                               heartbeat
        'X' reason:u8                     goodbye (2 = replaced by another client)
  C->S  [type:u8][len:u8][payload]
        'K' down:u8 name    'M' dx8:i16 dy8:i16 (1/8 view px)   'B' button down
        'W' v:i8 h:i8       'Z' zoom:i8 (+1 in, -1 out, 0 fit; 0 at fit toggles stretch)
        'Q' quality         'N' next monitor    'R' full refresh   'U' want sound:u8
        'C' show cursor:u8 (0 hides the pointer overlay)
        'A' frame ack id:u32   'P' ping
"""

import argparse
import hashlib
import hmac
import io
import json
import os
import secrets
import socket
import struct
import sys
import threading
import time

from PIL import Image, ImageDraw

VERSION = "1.1.0"
PROTO_VER = 3
PORT = 5901
TILE_W, TILE_H = 32, 16              # dirty-region grid (JPEG MCU friendly)
ZOOM_RATIOS = (4.0, 3.0, 2.0, 1.5, 1.0)  # screen px per panel px, below "fit"
QUALITY = ((45, 2), (70, 0), (88, 0))    # (jpeg quality, subsampling 2=4:2:0 0=4:4:4)
QUALITY_NAMES = ("Fast", "Normal", "Sharp")
MOTION_QUALITY_DROP = 30   # big changes (video, scrolling) go out at q-30, 4:2:0 ...
REFINE_AFTER = 0.25        # ... and get re-sent sharp once the screen holds still
AUDIO_RATE, AUDIO_BLOCK = 24000, 480
MAX_INFLIGHT = 2
CONFIG_PATH = os.path.join(os.path.expanduser("~"), ".cxl_remote.json")
DEFAULTS = {"pin": "", "require_pin": True, "port": PORT, "fps": 25, "stretch": False, "audio": True,
            "cursor": True}

try:
    LANCZOS = Image.Resampling.LANCZOS
except AttributeError:  # Pillow < 9.1
    LANCZOS = Image.LANCZOS

_log_sinks = []


def log(*a):
    t = time.time()
    line = time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t * 1000) % 1000:03d} " + " ".join(map(str, a))
    if not _log_sinks:
        print(line, flush=True)
    for sink in _log_sinks:
        sink(line)


def load_config():
    cfg = dict(DEFAULTS)
    try:
        with open(CONFIG_PATH, encoding="utf-8") as f:
            cfg.update(json.load(f))
    except (OSError, ValueError):
        pass
    if not cfg.get("pin"):
        cfg["pin"] = new_pin()
        save_config(cfg)
    return cfg


def save_config(cfg):
    try:
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=1)
    except OSError as e:
        log("could not save config:", e)


def new_pin():
    return f"{secrets.randbelow(10**6):06d}"


def list_monitors():
    import mss
    with (getattr(mss, "MSS", None) or mss.mss)() as sct:
        return sct.monitors[1:] or sct.monitors


# ---------------------------------------------------------------------------
# Input injection
# ---------------------------------------------------------------------------
if sys.platform == "win32":
    import ctypes
    from ctypes import wintypes

    try:  # physical pixels everywhere: capture, cursor and SetCursorPos agree
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass

    ULONG_PTR = ctypes.c_size_t

    class MOUSEINPUT(ctypes.Structure):
        _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                    ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]

    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                    ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]

    class HARDWAREINPUT(ctypes.Structure):
        _fields_ = [("uMsg", wintypes.DWORD), ("wParamL", wintypes.WORD), ("wParamH", wintypes.WORD)]

    class _INPUTUNION(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]

    class INPUT(ctypes.Structure):
        _fields_ = [("type", wintypes.DWORD), ("u", _INPUTUNION)]

    class WinInput:
        VK = {
            "backspace": 0x08, "tab": 0x09, "enter": 0x0D, "shift": 0xA0, "ctrl": 0xA2, "alt": 0xA4,
            "esc": 0x1B, "space": 0x20, "page_up": 0x21, "page_down": 0x22, "end": 0x23, "home": 0x24,
            "left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28, "print_screen": 0x2C,
            "insert": 0x2D, "delete": 0x2E, "cmd": 0x5B, "menu": 0x5D,
            **{f"f{i}": 0x6F + i for i in range(1, 13)},
        }
        EXTENDED = {"page_up", "page_down", "end", "home", "left", "up", "right", "down",
                    "print_screen", "insert", "delete", "cmd", "menu"}
        BTN = ((0x0002, 0x0004), (0x0008, 0x0010), (0x0020, 0x0040))

        def __init__(self):
            self.user32 = ctypes.WinDLL("user32", use_last_error=True)
            self.user32.SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
            self.user32.VkKeyScanW.argtypes = (wintypes.WCHAR,)
            self.user32.VkKeyScanW.restype = ctypes.c_short
            self.user32.MapVirtualKeyW.argtypes = (wintypes.UINT, wintypes.UINT)

        def _send(self, inp):
            self.user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))

        def key(self, name, down):
            flags = 0 if down else 0x0002  # KEYEVENTF_KEYUP
            vk = self.VK.get(name)
            if vk is None and len(name) == 1:
                res = self.user32.VkKeyScanW(name)
                if res == -1:  # not on this layout: type it as a unicode character
                    ki = KEYBDINPUT(0, ord(name), flags | 0x0004, 0, 0)
                    self._send(INPUT(1, _INPUTUNION(ki=ki)))
                    return
                vk = res & 0xFF  # shift state comes from the Cardputer's real Shift key
            if vk is None:
                return
            if name in self.EXTENDED:
                flags |= 0x0001
            scan = self.user32.MapVirtualKeyW(vk, 0)
            self._send(INPUT(1, _INPUTUNION(ki=KEYBDINPUT(vk, scan, flags, 0, 0))))

        def cursor_pos(self):
            pt = wintypes.POINT()
            self.user32.GetCursorPos(ctypes.byref(pt))
            return pt.x, pt.y

        def set_cursor(self, x, y):
            self.user32.SetCursorPos(int(x), int(y))

        def button(self, b, down):
            if 0 <= b < 3:
                mi = MOUSEINPUT(0, 0, 0, self.BTN[b][0 if down else 1], 0, 0)
                self._send(INPUT(0, _INPUTUNION(mi=mi)))

        def wheel(self, v, h):
            if v:
                self._send(INPUT(0, _INPUTUNION(mi=MOUSEINPUT(0, 0, ctypes.c_uint32(v * 120).value, 0x0800, 0, 0))))
            if h:
                self._send(INPUT(0, _INPUTUNION(mi=MOUSEINPUT(0, 0, ctypes.c_uint32(h * 120).value, 0x1000, 0, 0))))

    def make_input():
        return WinInput()
else:
    class X11Input:
        """Linux/X11 (e.g. Arduino UNO Q's XFCE session) via the XTEST
        extension. One X connection shared by the reader and render threads,
        so every call takes the lock."""
        KEYSYMS = {
            "backspace": "BackSpace", "tab": "Tab", "enter": "Return", "shift": "Shift_L",
            "ctrl": "Control_L", "alt": "Alt_L", "esc": "Escape", "space": "space",
            "page_up": "Prior", "page_down": "Next", "end": "End", "home": "Home",
            "left": "Left", "up": "Up", "right": "Right", "down": "Down", "print_screen": "Print",
            "insert": "Insert", "delete": "Delete", "cmd": "Super_L", "menu": "Menu",
            **{f"f{i}": f"F{i}" for i in range(1, 13)},
        }
        BUTTONS = (1, 3, 2)  # left, right, middle

        def __init__(self):
            from Xlib import X, XK, display
            from Xlib.ext import xtest
            self.X, self.XK, self.xtest = X, XK, xtest
            self.d = display.Display()
            self.root = self.d.screen().root
            self.lock = threading.Lock()

        def _fake(self, *args, **kw):
            self.xtest.fake_input(self.d, *args, **kw)
            self.d.sync()

        def key(self, name, down):
            # Single characters are the Cardputer's unshifted key caps; ASCII
            # keysyms equal their code points, and Shift arrives as its own key.
            sym = ord(name) if len(name) == 1 else self.XK.string_to_keysym(self.KEYSYMS.get(name, ""))
            with self.lock:
                code = self.d.keysym_to_keycode(sym) if sym else 0
                if code:
                    self._fake(self.X.KeyPress if down else self.X.KeyRelease, code)

        def cursor_pos(self):
            with self.lock:
                p = self.root.query_pointer()
                return p.root_x, p.root_y

        def set_cursor(self, x, y):
            with self.lock:
                self._fake(self.X.MotionNotify, x=int(x), y=int(y))

        def button(self, b, down):
            if 0 <= b < 3:
                with self.lock:
                    self._fake(self.X.ButtonPress if down else self.X.ButtonRelease, self.BUTTONS[b])

        def wheel(self, v, h):
            with self.lock:
                for btn, n in ((4 if v > 0 else 5, abs(v)), (7 if h > 0 else 6, abs(h))):
                    for _ in range(n):
                        self._fake(self.X.ButtonPress, btn)
                        self._fake(self.X.ButtonRelease, btn)

    def make_input():
        if os.environ.get("DISPLAY"):
            try:
                return X11Input()
            except ImportError:
                pass  # python3-xlib missing: fall back to pynput
        from pynput import keyboard, mouse

        class PynputInput:
            def __init__(self):
                self.kb = keyboard.Controller()
                self.ms = mouse.Controller()

            def _resolve(self, name):
                if len(name) == 1:
                    return keyboard.KeyCode.from_char(name)
                return getattr(keyboard.Key, name, None)

            def key(self, name, down):
                k = self._resolve(name)
                if k is not None:
                    (self.kb.press if down else self.kb.release)(k)

            def cursor_pos(self):
                x, y = self.ms.position
                return int(x), int(y)

            def set_cursor(self, x, y):
                self.ms.position = (int(x), int(y))

            def button(self, b, down):
                btn = (mouse.Button.left, mouse.Button.right, mouse.Button.middle)[b] if 0 <= b < 3 else None
                if btn:
                    (self.ms.press if down else self.ms.release)(btn)

            def wheel(self, v, h):
                self.ms.scroll(h, v)

        return PynputInput()


# ---------------------------------------------------------------------------
# Rendering helpers
# ---------------------------------------------------------------------------
CURSOR_SHAPE = [(0, 0), (0, 15), (4, 11), (7, 17), (9, 16), (6, 10), (11, 10)]


def draw_cursor(img, x, y):
    if -12 < x < img.width and -18 < y < img.height:
        d = ImageDraw.Draw(img)
        d.polygon([(x + px, y + py) for px, py in CURSOR_SHAPE], fill=(255, 255, 255), outline=(0, 0, 0))


def jpeg(img, quality, subsampling):
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=quality, subsampling=subsampling, optimize=False, progressive=False)
    return buf.getvalue()


def dirty_tiles(prev, cur, ow, oh):
    """Set of (tx, ty) grid cells that differ between two RGB byte buffers."""
    row = ow * 3
    cols = ow // TILE_W
    tiles = set()
    for by in range(oh // TILE_H):
        s, e = by * TILE_H * row, (by + 1) * TILE_H * row
        if prev is None:
            tiles.update((tx, by) for tx in range(cols))
            continue
        if prev[s:e] == cur[s:e]:
            continue
        for tx in range(cols):
            x0 = tx * TILE_W * 3
            for r in range(TILE_H):
                o = s + r * row + x0
                if prev[o:o + TILE_W * 3] != cur[o:o + TILE_W * 3]:
                    tiles.add((tx, by))
                    break
    return tiles


def tiles_to_rects(tiles, oh):
    """Merge grid cells into rectangles: per band row the changed column span,
    consecutive band rows joined (fewer JPEGs = less header overhead)."""
    spans = {}
    for tx, ty in tiles:
        lo, hi = spans.get(ty, (tx, tx))
        spans[ty] = (min(lo, tx), max(hi, tx))
    rects = []
    run = None
    for by in range(oh // TILE_H + 1):
        sp = spans.get(by)
        if sp is None:
            if run:
                rects.append((run[0] * TILE_W, run[2] * TILE_H, (run[1] + 1) * TILE_W, by * TILE_H))
            run = None
        elif run is None:
            run = [sp[0], sp[1], by]
        else:
            run[0], run[1] = min(run[0], sp[0]), max(run[1], sp[1])
    return rects


def encode_rect(img, rect, quality, subsampling, maxlen):
    x0, y0, x1, y1 = rect
    data = jpeg(img.crop(rect), quality, subsampling)
    if len(data) <= maxlen:
        return [(rect, data)]
    if y1 - y0 > TILE_H:
        mid = y0 + ((y1 - y0) // TILE_H // 2) * TILE_H
        return (encode_rect(img, (x0, y0, x1, mid), quality, subsampling, maxlen)
                + encode_rect(img, (x0, mid, x1, y1), quality, subsampling, maxlen))
    if x1 - x0 > TILE_W:
        mid = x0 + ((x1 - x0) // TILE_W // 2) * TILE_W
        return (encode_rect(img, (x0, y0, mid, y1), quality, subsampling, maxlen)
                + encode_rect(img, (mid, y0, x1, y1), quality, subsampling, maxlen))
    return encode_rect(img, rect, max(10, quality - 20), 2, maxlen) if quality > 10 else [(rect, data[:maxlen])]




# ---------------------------------------------------------------------------
# One Cardputer connection
# ---------------------------------------------------------------------------
class Session:
    def __init__(self, sock, addr, server):
        self.sock, self.addr, self.server = sock, addr, server
        self.inp = server.inp
        self.alive = True
        self.lock = threading.Condition()
        self.wake = threading.Event()
        self.zoom_idx = 0
        self.quality = 1
        self.mon_idx = server.monitor
        self.stretch = server.cfg["stretch"]
        self.force_full = True
        self.sent_id = 0
        self.acked_id = 0
        self.view = None              # (vx, vy) top-left of a zoomed view
        self.mouse_f = None           # float cursor position for sub-pixel moves
        self.keys_down = set()
        self.buttons_down = set()
        self.recenter_cursor = False
        self.cur_ratio, self.cur_mon = (1.0, 1.0), None
        self.cache_key, self.cache_raw, self.cache_img = None, None, None
        self.ow, self.oh, self.maxjpeg = 320, 240, 48 * 1024
        self.send_lock = threading.Lock()  # video + audio threads share the socket
        self.sound_wanted = False
        self.audio_active = False
        self.lowq = set()                   # tiles currently shown at motion quality
        # stats for the GUI
        self.connected_at = time.time()
        self.preview = None
        self.view_label = "FIT"
        self.fps = self.kbps = 0.0
        self._stat_frames = self._stat_bytes = 0
        self._stat_t0 = time.monotonic()

    # -- io --
    def recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("closed")
            buf += chunk
        return buf

    def send(self, data):
        with self.send_lock:
            self.sock.sendall(data)
            self._stat_bytes += len(data)

    def close(self, reason=None):
        if not self.alive:
            return
        self.alive = False
        # Goodbye only if the socket is free soon - never hang on a stuck client.
        if reason is not None and self.send_lock.acquire(timeout=0.5):
            try:
                self.sock.sendall(b"X" + bytes([reason]))
                self.sock.shutdown(socket.SHUT_WR)  # flush the goodbye before closing
            except OSError:
                pass
            finally:
                self.send_lock.release()
        try:
            self.sock.close()
        except OSError:
            pass
        with self.lock:
            self.lock.notify_all()
        self.wake.set()

    # -- settings changed from the GUI --
    def update(self, **kw):
        with self.lock:
            if "monitor" in kw and kw["monitor"] != self.mon_idx:
                self.mon_idx = kw["monitor"]
                self.recenter_cursor = True
                self.view = None
            if "stretch" in kw:
                self.stretch = kw["stretch"]
            if "quality" in kw:
                self.quality = kw["quality"]
            self.force_full = True
        self.wake.set()

    # -- handshake --
    def handshake(self, monitors):
        nonce = secrets.token_bytes(16)
        self.sock.settimeout(10)
        self.send(b"CXLS" + bytes([PROTO_VER]) + nonce)
        hello = self.recv_exact(45)
        if hello[:4] != b"CXLR" or hello[4] != PROTO_VER:
            self.send(b"X\x03")
            raise ConnectionError(f"protocol mismatch (client v{hello[4]}, server v{PROTO_VER}) - reflash the Cardputer")
        self.ow, self.oh, self.maxjpeg = struct.unpack("<HHI", hello[5:13])
        pin = self.server.cfg["pin"] if self.server.cfg["require_pin"] else ""
        if pin:
            expect = hmac.new(pin.encode(), nonce, hashlib.sha256).digest()
            if not hmac.compare_digest(expect, hello[13:45]):
                self.send(b"X\x01")
                raise ConnectionError("wrong PIN")
        name = socket.gethostname().encode()[:63]
        mon = monitors[self.mon_idx % len(monitors)]
        self.send(b"O" + struct.pack("<HHB", mon["width"], mon["height"], len(name)) + name)
        self.sock.settimeout(10)  # client pings every 2 s

    # -- incoming input --
    def reader(self):
        try:
            while self.alive:
                t, n = self.recv_exact(2)
                p = self.recv_exact(n) if n else b""
                self.handle(t, p)
        except (OSError, ConnectionError, struct.error) as e:
            if self.alive:
                log(f"{self.addr[0]}: disconnected ({e.__class__.__name__})")
        finally:
            self.release_all()
            self.close()

    def handle(self, t, p):
        inp = self.inp
        view_only = self.server.view_only
        if t in (ord("K"), ord("M"), ord("B"), ord("W")) and view_only:
            return
        if t == ord("K"):
            down, name = bool(p[0]), p[1:].decode("ascii", "replace")
            inp.key(name, down)
            (self.keys_down.add if down else self.keys_down.discard)(name)
            if self.server.verbose:
                log(f"key {name} {'down' if down else 'up'}")
        elif t == ord("M"):
            dx8, dy8 = struct.unpack("<hh", p)
            self.move_mouse(dx8 / 8.0, dy8 / 8.0)
            self.wake.set()
        elif t == ord("B"):
            b, down = p[0], bool(p[1])
            inp.button(b, down)
            (self.buttons_down.add if down else self.buttons_down.discard)(b)
            self.wake.set()
        elif t == ord("W"):
            v, h = struct.unpack("<bb", p)
            inp.wheel(v, h)
            self.wake.set()
        elif t == ord("Z"):
            z = struct.unpack("<b", p)[0]
            with self.lock:
                if z == 0 and self.zoom_idx == 0:
                    self.stretch = not self.stretch
                    self.server.remember(stretch=self.stretch)
                else:
                    self.zoom_idx = 0 if z == 0 else max(0, self.zoom_idx + z)
                self.view = None
                self.force_full = True
            self.wake.set()
        elif t == ord("Q"):
            with self.lock:
                self.quality = min(p[0], len(QUALITY) - 1)
                self.force_full = True
            self.wake.set()
        elif t == ord("N"):
            with self.lock:
                self.mon_idx += 1
                self.view = None
                self.force_full = True
                self.recenter_cursor = True
            self.wake.set()
        elif t == ord("R"):
            with self.lock:
                self.force_full = True
            self.wake.set()
        elif t == ord("C"):
            self.server.remember(cursor=bool(p[0]))
            with self.lock:
                self.force_full = True
            self.wake.set()
        elif t == ord("U"):
            self.sound_wanted = bool(p[0])
        elif t == ord("A"):
            with self.lock:
                self.acked_id = max(self.acked_id, struct.unpack("<I", p)[0])
                self.lock.notify_all()

    def release_all(self):
        for name in list(self.keys_down):
            try:
                self.inp.key(name, False)
            except Exception:
                pass
        for b in list(self.buttons_down):
            try:
                self.inp.button(b, False)
            except Exception:
                pass
        self.keys_down.clear()
        self.buttons_down.clear()

    def move_mouse(self, dvx, dvy):
        # dv* are in panel pixels; convert with the current view scale so the
        # pointer moves at the same visual speed at every zoom level.
        (rx, ry), mon = self.cur_ratio, self.cur_mon
        cx, cy = self.inp.cursor_pos()
        if self.mouse_f is None or abs(self.mouse_f[0] - cx) > 1.5 or abs(self.mouse_f[1] - cy) > 1.5:
            self.mouse_f = (float(cx), float(cy))  # physical mouse moved meanwhile
        x, y = self.mouse_f[0] + dvx * rx, self.mouse_f[1] + dvy * ry
        if mon:
            x = min(max(x, mon["left"]), mon["left"] + mon["width"] - 1)
            y = min(max(y, mon["top"]), mon["top"] + mon["height"] - 1)
        self.mouse_f = (x, y)
        self.inp.set_cursor(round(x), round(y))

    # -- outgoing frames --
    def render(self, sct, monitors):
        with self.lock:
            self.mon_idx %= len(monitors)
            mon = monitors[self.mon_idx]
            recenter = self.recenter_cursor
            self.recenter_cursor = False
        L, T, W, H = mon["left"], mon["top"], mon["width"], mon["height"]
        if recenter:
            self.inp.set_cursor(L + W // 2, T + H // 2)
            self.mouse_f = None
        ow, oh = self.ow, self.oh
        fit = max(W / ow, H / oh)
        ratios = [fit] + [r for r in ZOOM_RATIOS if r < fit - 1e-6]
        with self.lock:
            self.zoom_idx = min(self.zoom_idx, len(ratios) - 1)
            zi = self.zoom_idx
            stretch = self.stretch and zi == 0
        cx, cy = self.inp.cursor_pos()

        if stretch:  # whole monitor squeezed onto the whole panel, no black bars
            rx, ry = W / ow, H / oh
            vx, vy, vw, vh = L, T, W, H
        else:
            rx = ry = ratios[zi]
            vw, vh = round(ow * rx), round(oh * ry)
            if zi == 0:
                vx, vy = L + (W - vw) // 2, T + (H - vh) // 2
            else:
                if self.view is None:
                    vx, vy = cx - vw // 2, cy - vh // 2
                else:
                    vx, vy = self.view
                    mx, my = vw * 0.15, vh * 0.15   # follow the pointer with a margin
                    if cx < vx + mx:
                        vx = int(cx - mx)
                    elif cx > vx + vw - mx:
                        vx = int(cx + mx - vw)
                    if cy < vy + my:
                        vy = int(cy - my)
                    elif cy > vy + vh - my:
                        vy = int(cy + my - vh)
                vx = L + (W - vw) // 2 if vw >= W else min(max(vx, L), L + W - vw)
                vy = T + (H - vh) // 2 if vh >= H else min(max(vy, T), T + H - vh)
                self.view = (vx, vy)

        ix0, iy0 = max(vx, L), max(vy, T)
        ix1, iy1 = min(vx + vw, L + W), min(vy + vh, T + H)
        canvas = Image.new("RGB", (ow, oh))
        if ix1 > ix0 and iy1 > iy0:
            shot = sct.grab({"left": ix0, "top": iy0, "width": ix1 - ix0, "height": iy1 - iy0})
            raw = shot.bgra
            key = (ix0, iy0, ix1, iy1, rx, ry)
            if key == self.cache_key and raw == self.cache_raw:
                img = self.cache_img  # screen unchanged: skip the costly downscale
            else:
                img = Image.frombytes("RGB", shot.size, raw, "raw", "BGRX")
                dw, dh = max(1, round((ix1 - ix0) / rx)), max(1, round((iy1 - iy0) / ry))
                if (dw, dh) != img.size:
                    img = img.resize((dw, dh), LANCZOS, reducing_gap=2.0)
                self.cache_key, self.cache_raw, self.cache_img = key, raw, img
            canvas.paste(img, (round((ix0 - vx) / rx), round((iy0 - vy) / ry)))
        if self.server.cfg["cursor"]:
            draw_cursor(canvas, round((cx - vx) / rx), round((cy - vy) / ry))
        self.cur_ratio, self.cur_mon = (rx, ry), mon
        self.view_label = "STRETCH" if stretch else ("FIT" if zi == 0 else f"1:{rx:g}")
        return canvas, max(rx, ry), zi, len(ratios), stretch

    def run(self):
        import mss  # created in this thread: mss handles are thread-bound on Windows
        with (getattr(mss, "MSS", None) or mss.mss)() as sct:
            monitors = sct.monitors[1:] or sct.monitors
            self.handshake(monitors)
            log(f"{self.addr[0]}: connected ({self.ow}x{self.oh} panel)")
            threading.Thread(target=self.reader, daemon=True).start()
            prev = None
            last_send = last_change = time.monotonic()
            last_motion = 0.0
            while self.alive:
                with self.lock:
                    while self.alive and self.sent_id - self.acked_id >= MAX_INFLIGHT:
                        if not self.lock.wait(5):
                            break
                    if not self.alive:
                        break
                    if self.force_full:
                        prev = None
                        self.lowq = set()
                        self.force_full = False
                    q, sub = QUALITY[self.quality]
                t0 = time.monotonic()
                try:
                    frame, ratio, zi, zc, stretch = self.render(sct, monitors)
                except Exception as e:  # e.g. display mode change / secure desktop
                    log("capture failed:", e)
                    time.sleep(0.5)
                    continue
                cur = frame.tobytes()
                tiles = dirty_tiles(prev, cur, self.ow, self.oh)
                now = time.monotonic()
                total = (self.ow // TILE_W) * (self.oh // TILE_H)
                if tiles:
                    # Video / scrolling: lots of the screen changes -> cheaper,
                    # faster-to-decode JPEGs; remember those tiles for a sharp
                    # re-send once things settle.
                    motion = prev is not None and (len(tiles) > total * 0.3 or
                                                   (now - last_motion < 0.5 and len(tiles) > total * 0.08))
                    if motion:
                        last_motion = now
                        enc_q, enc_sub = max(25, q - MOTION_QUALITY_DROP), 2
                        self.lowq |= tiles
                    else:
                        enc_q, enc_sub = q, sub
                        self.lowq -= tiles
                    rects = tiles_to_rects(tiles, self.oh)
                    last_change = now
                elif self.lowq and now - last_change > REFINE_AFTER:
                    rects = tiles_to_rects(self.lowq, self.oh)
                    enc_q, enc_sub = q, sub
                    self.lowq = set()
                else:
                    rects = []
                try:
                    if rects:
                        for rect in rects:
                            for (x0, y0, x1, y1), data in encode_rect(frame, rect, enc_q, enc_sub, self.maxjpeg):
                                self.send(b"T" + struct.pack("<HHHHI", x0, y0, x1 - x0, y1 - y0, len(data)) + data)
                        self.sent_id += 1
                        flags = ((1 if stretch else 0) | (2 if self.audio_active else 0) |
                                 (0 if self.server.cfg["cursor"] else 4))
                        self.send(b"F" + struct.pack("<IHBBBBB", self.sent_id, min(65535, round(ratio * 100)),
                                                     zi, zc, self.quality, self.mon_idx % len(monitors) + 1, flags))
                        self._stat_frames += 1
                        self.preview = frame
                        prev = cur
                        last_send = time.monotonic()
                    elif time.monotonic() - last_send > 2:
                        self.send(b"H")
                        last_send = time.monotonic()
                except OSError:
                    break
                now = time.monotonic()
                if now - self._stat_t0 >= 1:
                    self.fps = self._stat_frames / (now - self._stat_t0)
                    self.kbps = self._stat_bytes / 1024 / (now - self._stat_t0)
                    self._stat_frames = self._stat_bytes = 0
                    self._stat_t0 = now
                # pace: at most cfg fps, but wake early on input so the pointer feels live
                min_dt = 1.0 / max(1, self.server.cfg["fps"])
                self.wake.wait(max(0.0, (min_dt if rects else 0.04) - (now - t0)))
                self.wake.clear()
        self.close()


# ---------------------------------------------------------------------------
# Server: discovery + accept loop, shared by the console and GUI front-ends
# ---------------------------------------------------------------------------
class RemoteServer:
    def __init__(self, cfg=None, verbose=False):
        self.cfg = cfg or load_config()
        self.inp = make_input()
        self.verbose = verbose
        self.view_only = False
        self.monitor = 0
        self.session = None
        self.running = False
        self._tcp = self._udp = None
        self._audio_thread = None

    def remember(self, **kw):
        """Persist settings (also used when the Cardputer toggles stretch).
        Only these keys are written, so one-off command-line overrides stay one-off."""
        self.cfg.update(kw)
        disk = load_config()
        disk.update(kw)
        save_config(disk)

    def set_option(self, **kw):
        """GUI changes: apply to the live session and remember the defaults."""
        if "monitor" in kw:
            self.monitor = kw["monitor"]
        persist = {k: v for k, v in kw.items() if k in DEFAULTS}
        if persist:
            self.remember(**persist)
        s = self.session
        if s and s.alive:
            s.update(**{k: v for k, v in kw.items() if k in ("monitor", "stretch", "quality", "cursor")})

    def start(self):
        port = int(self.cfg["port"])
        tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        tcp.bind(("", port))
        tcp.listen(2)
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        udp.bind(("", port))
        self._tcp, self._udp, self.running = tcp, udp, True
        threading.Thread(target=self._discovery, args=(udp, port), daemon=True).start()
        threading.Thread(target=self._accept, args=(tcp,), daemon=True).start()
        if self._audio_thread is None:
            self._audio_thread = threading.Thread(target=self._audio_loop, daemon=True)
            self._audio_thread.start()
        log(f"listening on port {port}")

    def stop(self):
        self.running = False
        for s in (self._tcp, self._udp):
            try:
                s and s.close()
            except OSError:
                pass
        self.disconnect()
        log("server stopped")

    def disconnect(self):
        s = self.session
        if s and s.alive:
            log(f"{s.addr[0]}: disconnected by server")
            s.close(reason=0)

    def _audio_loop(self):
        """PC sound (WASAPI loopback of the default output) -> the connected
        Cardputer, as 20 ms blocks of 24 kHz mono mu-law. One thread for the
        server's lifetime: soundcard sets COM up in the thread that imports it."""
        try:
            import warnings
            warnings.filterwarnings("ignore")  # soundcard warns on every loopback discontinuity
            import numpy as np
            import soundcard as sc
        except Exception as e:
            log(f"sound unavailable ({e}) - pip install soundcard numpy")
            return
        k = 1.0 / np.log1p(255.0)
        while True:
            s = self.session
            if not (s and s.alive and s.sound_wanted and self.cfg["audio"]):
                time.sleep(0.1)
                continue
            try:
                spk = sc.default_speaker()
                mic = sc.get_microphone(id=str(spk.name), include_loopback=True)
                with mic.recorder(samplerate=AUDIO_RATE, channels=1, blocksize=AUDIO_BLOCK) as rec:
                    s.audio_active = True
                    log(f"sound: streaming '{spk.name}'")
                    while s.alive and s.sound_wanted and self.cfg["audio"] and self.session is s:
                        x = np.clip(rec.record(numframes=AUDIO_BLOCK)[:, 0], -1.0, 1.0)
                        y = np.sign(x) * np.log1p(255.0 * np.abs(x)) * k
                        q = np.round((y + 1.0) * 127.5).astype(np.uint8).tobytes()
                        s.send(b"S" + struct.pack("<H", len(q)) + q)
            except OSError:
                pass  # that session's socket closed
            except Exception as e:
                log("sound error:", e)
                time.sleep(2)
            finally:
                s.audio_active = False

    def _discovery(self, udp, port):
        name = socket.gethostname().encode()[:40]
        while self.running:
            try:
                data, addr = udp.recvfrom(64)
                if data.startswith(b"CXLR?"):
                    udp.sendto(b"CXLR!" + struct.pack("<H", port) + name, addr)
            except OSError:
                if not self.running:
                    return
                time.sleep(0.5)

    def _accept(self, tcp):
        while self.running:
            try:
                sock, addr = tcp.accept()
            except OSError:
                return
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            old = self.session
            if old and old.alive:
                log(f"{old.addr[0]}: replaced by {addr[0]}")
                old.close(reason=2)
            sess = Session(sock, addr, self)
            self.session = sess
            threading.Thread(target=self._run_session, args=(sess,), daemon=True).start()

    @staticmethod
    def _run_session(s):
        try:
            s.run()
        except (OSError, ConnectionError, struct.error) as e:
            log(f"{s.addr[0]}: {e}")
            s.close()


def local_ips():
    ips = set()
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 1))
            ips.add(s.getsockname()[0])
    except OSError:
        pass
    return sorted(ips) or ["?"]


def main():
    ap = argparse.ArgumentParser(description="CXL Remote server for the Cardputer XL (console mode)")
    ap.add_argument("--port", type=int, help=f"TCP/UDP port (default {PORT})")
    ap.add_argument("--pin", help="use this PIN for this run")
    ap.add_argument("--no-auth", action="store_true", help="accept any client (trusted networks only!)")
    ap.add_argument("--fps", type=float, help="frame rate cap (default 25)")
    ap.add_argument("--stretch", action="store_true", help="start in stretch-to-fit mode")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every key event")
    args = ap.parse_args()

    cfg = load_config()
    if args.port:
        cfg["port"] = args.port
    if args.pin:
        cfg["pin"] = args.pin
    if args.fps:
        cfg["fps"] = args.fps
    if args.stretch:
        cfg["stretch"] = True
    cfg["require_pin"] = not args.no_auth  # command-line overrides are not saved

    server = RemoteServer(cfg, verbose=args.verbose)
    server.start()
    print("=" * 52)
    print(f"  CXL Remote server {VERSION}  -  {socket.gethostname()}")
    print(f"  address : {', '.join(local_ips())}  port {cfg['port']}")
    print(f"  PIN     : {cfg['pin'] if cfg['require_pin'] else '(none - auth disabled)'}")
    print("=" * 52, flush=True)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
