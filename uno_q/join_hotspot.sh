#!/bin/bash
# Make the UNO Q use the Cardputer's hotspot (CXL Remote "Hotspot for PC").
# The home Wi-Fi stays saved as the fallback for when the hotspot is off.
#
#   bash ~/cxlremote/join_hotspot.sh "<hotspot name>" "<hotspot password>" [now]
#
# Pass "now" to switch immediately; otherwise NetworkManager prefers the
# hotspot (priority 10) the next time it (re)connects. While the UNO Q sits
# behind the Cardputer's NAT, other machines on the home network can no
# longer reach it (no SSH from the PC) - the Cardputer is its only peer.
set -e
SSID="$1"; PASS="$2"
if [ -z "$SSID" ] || [ ${#PASS} -lt 8 ]; then
  echo "usage: $0 <ssid> <password (8+ chars)> [now]"; exit 1
fi
nmcli connection delete cxl-hotspot >/dev/null 2>&1 || true
nmcli connection add type wifi ifname wlan0 con-name cxl-hotspot ssid "$SSID" \
  wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$PASS" \
  connection.autoconnect yes connection.autoconnect-priority 10 >/dev/null
echo "saved hotspot profile 'cxl-hotspot' for $SSID"
if [ "$3" = "now" ]; then
  nmcli connection up cxl-hotspot
fi
