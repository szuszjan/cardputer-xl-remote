#!/usr/bin/env python3
"""CXL Remote - server window.

Double-click (pythonw, no console) or: python cxl_remote_gui.pyw
Shows a live preview of exactly what the Cardputer XL panel gets, the PIN,
connection stats, and lets you pick monitor / fit mode / quality / fps,
switch to view-only, or kick the Cardputer off.
"""

import os
import queue
import socket
import sys
import tkinter as tk
from tkinter import ttk

from PIL import Image, ImageDraw, ImageFont, ImageTk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cxl_remote_server as core  # noqa: E402

BG, PANEL, FG, DIM, ACCENT, OK, WARN = "#14191c", "#1e2629", "#e3ebed", "#8ca0a4", "#2dbbc7", "#53d3b3", "#f0a050"
PREVIEW_SCALE = 1.5


class App:
    def __init__(self, root):
        self.root = root
        self.logq = queue.Queue()
        core._log_sinks.append(self.logq.put)
        self.server = core.RemoteServer()
        self.monitors = core.list_monitors()
        self.photo = None
        self.last_preview = None
        self.pin_visible = True

        root.title(f"CXL Remote Server {core.VERSION}")
        icon = os.path.join(getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__))), "icon.ico")
        try:
            root.iconbitmap(icon)
        except tk.TclError:
            pass
        root.configure(bg=BG)
        root.minsize(860, 560)
        self._style()
        self._build()
        self.start_server()
        self.root.protocol("WM_DELETE_WINDOW", self.quit)
        self.tick()

    # ---------------------------------------------------------------- layout
    def _style(self):
        st = ttk.Style(self.root)
        st.theme_use("clam")
        st.configure(".", background=BG, foreground=FG, fieldbackground=PANEL, bordercolor="#313b3f",
                     lightcolor=PANEL, darkcolor=PANEL, troughcolor=PANEL, font=("Segoe UI", 10))
        st.configure("TLabelframe", background=BG, bordercolor="#313b3f")
        st.configure("TLabelframe.Label", background=BG, foreground=ACCENT, font=("Segoe UI Semibold", 10))
        st.configure("TButton", background=PANEL, foreground=FG, padding=(10, 4))
        st.map("TButton", background=[("active", "#2a3438"), ("disabled", BG)], foreground=[("disabled", DIM)])
        st.configure("Accent.TButton", background="#168e9a", foreground="white")
        st.map("Accent.TButton", background=[("active", "#1aa5b2")])
        st.configure("TCheckbutton", background=BG, foreground=FG)
        st.map("TCheckbutton", background=[("active", BG)])
        st.configure("TRadiobutton", background=BG, foreground=FG)
        st.map("TRadiobutton", background=[("active", BG)])
        st.configure("TCombobox", foreground=FG, arrowcolor=FG, selectbackground=PANEL, selectforeground=FG,
                     background=PANEL)
        st.map("TCombobox", fieldbackground=[("readonly", PANEL)], foreground=[("readonly", FG)],
               selectbackground=[("readonly", PANEL)], selectforeground=[("readonly", FG)],
               background=[("active", "#2a3438"), ("readonly", PANEL)])
        st.configure("Dim.TLabel", foreground=DIM)
        st.configure("Pin.TLabel", font=("Consolas", 26, "bold"), foreground=ACCENT)
        st.configure("Status.TLabel", font=("Segoe UI Semibold", 11))
        st.configure("Horizontal.TScale", background=BG)
        self.root.option_add("*TCombobox*Listbox.background", PANEL)
        self.root.option_add("*TCombobox*Listbox.foreground", FG)

    def _build(self):
        main = ttk.Frame(self.root, padding=12)
        main.pack(fill="both", expand=True)
        main.columnconfigure(0, weight=1)
        main.rowconfigure(1, weight=1)

        # ---- left: preview + live status
        left = ttk.Frame(main)
        left.grid(row=0, column=0, sticky="nw")
        pw, ph = int(320 * PREVIEW_SCALE), int(240 * PREVIEW_SCALE)
        self.preview = tk.Label(left, bg="black", width=pw, height=ph, bd=0, highlightthickness=1,
                                highlightbackground="#313b3f")
        self.preview.pack()
        self.status = ttk.Label(left, text="", style="Status.TLabel")
        self.status.pack(anchor="w", pady=(8, 0))
        self.stats = ttk.Label(left, text="", style="Dim.TLabel")
        self.stats.pack(anchor="w")
        ttk.Label(left, text="Preview = exactly what the Cardputer XL panel shows (320x240).",
                  style="Dim.TLabel").pack(anchor="w", pady=(2, 0))

        # ---- right: settings
        right = ttk.Frame(main)
        right.grid(row=0, column=1, sticky="nsew", padx=(14, 0))

        srv = ttk.LabelFrame(right, text="Server", padding=8)
        srv.pack(fill="x")
        ttk.Label(srv, text=f"{socket.gethostname()}   {', '.join(core.local_ips())} : {self.server.cfg['port']}").pack(anchor="w")
        row = ttk.Frame(srv)
        row.pack(fill="x", pady=(6, 0))
        self.btn_start = ttk.Button(row, text="Stop server", command=self.toggle_server)
        self.btn_start.pack(side="left")
        self.btn_kick = ttk.Button(row, text="Disconnect Cardputer", command=self.server.disconnect)
        self.btn_kick.pack(side="left", padx=6)

        pin = ttk.LabelFrame(right, text="PIN  (enter once on the Cardputer)", padding=8)
        pin.pack(fill="x", pady=(10, 0))
        self.pin_label = ttk.Label(pin, text="", style="Pin.TLabel")
        self.pin_label.pack(anchor="w")
        row = ttk.Frame(pin)
        row.pack(fill="x", pady=(4, 0))
        ttk.Button(row, text="Copy", command=self.copy_pin).pack(side="left")
        ttk.Button(row, text="New PIN", command=self.new_pin).pack(side="left", padx=6)
        self.btn_show = ttk.Button(row, text="Hide", command=self.toggle_pin)
        self.btn_show.pack(side="left")
        self.var_require = tk.BooleanVar(value=self.server.cfg["require_pin"])
        ttk.Checkbutton(pin, text="Require PIN (turn off only on a trusted network)", variable=self.var_require,
                        command=lambda: self.server.set_option(require_pin=self.var_require.get())).pack(anchor="w", pady=(6, 0))

        disp = ttk.LabelFrame(right, text="Display", padding=8)
        disp.pack(fill="x", pady=(10, 0))
        disp.columnconfigure(1, weight=1)
        ttk.Label(disp, text="Monitor").grid(row=0, column=0, sticky="w")
        names = [f"{i + 1}:  {m['width']}x{m['height']}  at {m['left']},{m['top']}" for i, m in enumerate(self.monitors)]
        self.var_mon = tk.StringVar(value=names[0] if names else "")
        cb = ttk.Combobox(disp, values=names, textvariable=self.var_mon, state="readonly", width=26)
        cb.grid(row=0, column=1, sticky="ew", padx=(8, 0))
        cb.bind("<<ComboboxSelected>>", lambda e: self.server.set_option(monitor=cb.current()))
        self.cb_mon = cb

        ttk.Label(disp, text="Fit mode").grid(row=1, column=0, sticky="w", pady=(8, 0))
        fit = ttk.Frame(disp)
        fit.grid(row=1, column=1, sticky="w", padx=(8, 0), pady=(8, 0))
        self.var_stretch = tk.BooleanVar(value=self.server.cfg["stretch"])
        ttk.Radiobutton(fit, text="Keep aspect (bars)", value=False, variable=self.var_stretch,
                        command=self.set_stretch).pack(side="left")
        ttk.Radiobutton(fit, text="Stretch to fit", value=True, variable=self.var_stretch,
                        command=self.set_stretch).pack(side="left", padx=(10, 0))

        ttk.Label(disp, text="Quality").grid(row=2, column=0, sticky="w", pady=(8, 0))
        self.var_q = tk.StringVar(value=core.QUALITY_NAMES[1])
        qb = ttk.Combobox(disp, values=core.QUALITY_NAMES, textvariable=self.var_q, state="readonly", width=10)
        qb.grid(row=2, column=1, sticky="w", padx=(8, 0), pady=(8, 0))
        qb.bind("<<ComboboxSelected>>", lambda e: self.server.set_option(quality=qb.current()))
        self.cb_q = qb

        ttk.Label(disp, text="Max FPS").grid(row=3, column=0, sticky="w", pady=(8, 0))
        fr = ttk.Frame(disp)
        fr.grid(row=3, column=1, sticky="ew", padx=(8, 0), pady=(8, 0))
        self.var_fps = tk.IntVar(value=int(self.server.cfg["fps"]))
        self.lbl_fps = ttk.Label(fr, text=str(self.var_fps.get()), width=3)
        sc = ttk.Scale(fr, from_=5, to=30, orient="horizontal", command=self.set_fps)
        sc.set(self.var_fps.get())
        sc.pack(side="left", fill="x", expand=True)
        self.lbl_fps.pack(side="left", padx=(6, 0))

        ctl = ttk.LabelFrame(right, text="Control", padding=8)
        ctl.pack(fill="x", pady=(10, 0))
        self.var_viewonly = tk.BooleanVar(value=False)
        ttk.Checkbutton(ctl, text="View only (ignore Cardputer keyboard / mouse)", variable=self.var_viewonly,
                        command=lambda: setattr(self.server, "view_only", self.var_viewonly.get())).pack(anchor="w")
        self.var_verbose = tk.BooleanVar(value=False)
        ttk.Checkbutton(ctl, text="Log every key", variable=self.var_verbose,
                        command=lambda: setattr(self.server, "verbose", self.var_verbose.get())).pack(anchor="w")

        # ---- bottom: log
        logf = ttk.LabelFrame(main, text="Log", padding=6)
        logf.grid(row=1, column=0, columnspan=2, sticky="nsew", pady=(12, 0))
        self.log = tk.Text(logf, height=7, bg=PANEL, fg=FG, insertbackground=FG, bd=0, font=("Consolas", 9),
                           state="disabled", wrap="none")
        sb = ttk.Scrollbar(logf, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set)
        self.log.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.update_pin()

    # --------------------------------------------------------------- actions
    def start_server(self):
        try:
            self.server.start()
        except OSError as e:
            core.log(f"cannot listen on port {self.server.cfg['port']}: {e} (another server running?)")

    def toggle_server(self):
        if self.server.running:
            self.server.stop()
        else:
            self.start_server()

    def set_stretch(self):
        self.server.set_option(stretch=self.var_stretch.get())

    def set_fps(self, v):
        fps = int(float(v))
        self.lbl_fps.configure(text=str(fps))
        if fps != self.server.cfg["fps"]:
            self.server.set_option(fps=fps)

    def update_pin(self):
        pin = self.server.cfg["pin"]
        self.pin_label.configure(text=" ".join(pin) if self.pin_visible else "• " * len(pin))

    def toggle_pin(self):
        self.pin_visible = not self.pin_visible
        self.btn_show.configure(text="Hide" if self.pin_visible else "Show")
        self.update_pin()

    def copy_pin(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.server.cfg["pin"])

    def new_pin(self):
        self.server.set_option(pin=core.new_pin())
        self.update_pin()
        core.log("new PIN generated - the Cardputer will ask for it on its next connect")

    def quit(self):
        try:
            self.server.stop()
        finally:
            self.root.destroy()

    # ------------------------------------------------------------- refresh
    def placeholder(self, text):
        pw, ph = int(320 * PREVIEW_SCALE), int(240 * PREVIEW_SCALE)
        img = Image.new("RGB", (pw, ph), (12, 15, 17))
        d = ImageDraw.Draw(img)
        try:
            font = ImageFont.load_default(size=18)
        except TypeError:  # Pillow < 10.1
            font = ImageFont.load_default()
        d.text((pw // 2, ph // 2), text, fill=(120, 140, 145), anchor="mm", font=font)
        return img

    def tick(self):
        while True:
            try:
                line = self.logq.get_nowait()
            except queue.Empty:
                break
            self.log.configure(state="normal")
            self.log.insert("end", line + "\n")
            if int(self.log.index("end-1c").split(".")[0]) > 500:
                self.log.delete("1.0", "100.0")
            self.log.see("end")
            self.log.configure(state="disabled")

        s = self.server.session
        live = s is not None and s.alive
        self.btn_start.configure(text="Stop server" if self.server.running else "Start server")
        self.btn_kick.state(["!disabled"] if live else ["disabled"])
        if not self.server.running:
            self.status.configure(text="Server stopped", foreground=WARN)
            self.stats.configure(text="")
        elif live:
            up = int(core.time.time() - s.connected_at)
            self.status.configure(text=f"Connected: Cardputer {s.addr[0]}" + ("   (view only)" if self.server.view_only else ""),
                                  foreground=OK)
            self.stats.configure(text=f"{s.fps:4.1f} fps   {s.kbps:5.0f} KB/s   {s.view_label}   "
                                      f"monitor {s.mon_idx % max(1, len(self.monitors)) + 1}   "
                                      f"quality {core.QUALITY_NAMES[s.quality]}   {up // 60}:{up % 60:02d}")
            # follow changes the Cardputer made itself
            if self.var_stretch.get() != s.stretch:
                self.var_stretch.set(s.stretch)
            if self.cb_q.current() != s.quality:
                self.cb_q.current(s.quality)
            mi = s.mon_idx % max(1, len(self.monitors))
            if self.cb_mon.current() != mi and self.monitors:
                self.cb_mon.current(mi)
        else:
            self.status.configure(text="Waiting for the Cardputer...", foreground=ACCENT)
            self.stats.configure(text="")

        frame = s.preview if live else None
        if frame is not self.last_preview or self.photo is None:
            self.last_preview = frame
            if frame is not None:
                img = frame.resize((int(320 * PREVIEW_SCALE), int(240 * PREVIEW_SCALE)), Image.NEAREST)
            else:
                img = self.placeholder("no Cardputer connected" if self.server.running else "server stopped")
            self.photo = ImageTk.PhotoImage(img)
            self.preview.configure(image=self.photo, width=img.width, height=img.height)
        self.root.after(150, self.tick)


def main():
    root = tk.Tk()
    try:  # crisp text on high-DPI Windows displays
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        pass
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
