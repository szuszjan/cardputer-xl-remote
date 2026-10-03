#!/bin/bash
# CXL Remote on an Arduino UNO Q: turns the board into a headless mini PC
# whose screen, sound and keyboard are the Cardputer XL.
#
# Run once, as root:   sudo bash ~/cxlremote/setup.sh [640x480|800x600]
#
#  - installs Python deps (Debian packages + mss/soundcard in a venv)
#  - Xorg "dummy" driver = a virtual monitor (default 640x480: exactly 2x the
#    Cardputer's 320x240, same 4:3 shape, so FIT has no black bars).
#    NOTE: while /etc/X11/xorg.conf.d/90-cxlremote-dummy.conf exists, a real
#    monitor on the USB-C port stays dark - delete that file to undo.
#  - XFCE autostart entry that launches the server with the desktop session
set -e

USER_NAME=${SUDO_USER:-arduino}
HOME_DIR=$(getent passwd "$USER_NAME" | cut -d: -f6)
APP="$HOME_DIR/cxlremote"
RES=${1:-640x480}

if [ "$(id -u)" != 0 ]; then echo "run with sudo"; exit 1; fi

echo "== packages"
apt-get update
apt-get install -y python3-pil python3-numpy python3-xlib python3-cffi python3-venv \
                   xserver-xorg-video-dummy pulseaudio-utils x11-xserver-utils

echo "== python venv ($APP/venv)"
sudo -u "$USER_NAME" python3 -m venv --system-site-packages "$APP/venv"
sudo -u "$USER_NAME" "$APP/venv/bin/pip" install --quiet --upgrade mss soundcard

echo "== virtual screen $RES"
case "$RES" in
  640x480)  MODELINE='"640x480_60.00"  23.75  640 664 720 800  480 483 487 500 -hsync +vsync' ;;
  800x600)  MODELINE='"800x600_60.00"  38.25  800 832 912 1024  600 603 607 624 -hsync +vsync' ;;
  *) echo "resolution must be 640x480 or 800x600"; exit 1 ;;
esac
MODE=$(echo "$MODELINE" | cut -d'"' -f2)
mkdir -p /etc/X11/xorg.conf.d
cat > /etc/X11/xorg.conf.d/90-cxlremote-dummy.conf <<EOF
# CXL Remote: headless virtual monitor for the Cardputer XL. Delete this file
# to use a real monitor again.
Section "Device"
    Identifier "CXLDummy"
    Driver "dummy"
    VideoRam 32768
EndSection
Section "Monitor"
    Identifier "CXLMonitor"
    HorizSync 28.0-80.0
    VertRefresh 48.0-75.0
    Modeline $MODELINE
EndSection
Section "Screen"
    Identifier "CXLScreen"
    Device "CXLDummy"
    Monitor "CXLMonitor"
    DefaultDepth 24
    SubSection "Display"
        Depth 24
        Modes "$MODE"
    EndSubSection
EndSection
EOF

echo "== desktop autologin"
if ! grep -rqs "^autologin-user=" /etc/lightdm/; then
  mkdir -p /etc/lightdm/lightdm.conf.d
  printf '[Seat:*]\nautologin-user=%s\n' "$USER_NAME" > /etc/lightdm/lightdm.conf.d/90-cxlremote.conf
fi

echo "== autostart"
chmod +x "$APP/start.sh"
sudo -u "$USER_NAME" mkdir -p "$HOME_DIR/.config/autostart"
cat > "$HOME_DIR/.config/autostart/cxlremote.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=CXL Remote server
Comment=Cardputer XL remote desktop server
Exec=$APP/start.sh
X-GNOME-Autostart-enabled=true
EOF
chown "$USER_NAME:" "$HOME_DIR/.config/autostart/cxlremote.desktop"

echo
echo "Done. Reboot (sudo reboot); the server starts with the desktop."
echo "PIN for the Cardputer: cat ~/.cxl_remote.json   (after the first start)"
