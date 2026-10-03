#!/bin/bash
# Launched by the XFCE autostart entry (setup.sh) inside the desktop session,
# so DISPLAY/XAUTHORITY and the PulseAudio/PipeWire session are already set.
cd "$(dirname "$0")"

# Never blank or lock the virtual screen - the Cardputer is the only monitor.
xset s off -dpms 2>/dev/null
xfconf-query -c xfce4-screensaver -p /saver/enabled -n -t bool -s false 2>/dev/null
xfconf-query -c xfce4-screensaver -p /lock/enabled -n -t bool -s false 2>/dev/null
xfconf-query -c xfce4-power-manager -p /xfce4-power-manager/dpms-enabled -n -t bool -s false 2>/dev/null

# The board has no speaker: give the desktop a virtual output whose monitor
# the server streams to the Cardputer.
if pactl info >/dev/null 2>&1; then
  pactl list short sinks | grep -q cxl_out ||
    pactl load-module module-null-sink sink_name=cxl_out sink_properties=device.description=CXL_Remote >/dev/null
  pactl set-default-sink cxl_out
fi

# Back onto the Cardputer's hotspot whenever it reappears (see the script).
pgrep -f "[h]otspot_watch.sh" >/dev/null || (setsid ./hotspot_watch.sh >/dev/null 2>&1 &)

exec ./venv/bin/python cxl_remote_server.py --fps 20 >> server.log 2>&1
