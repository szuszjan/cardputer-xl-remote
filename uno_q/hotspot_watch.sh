#!/bin/bash
# Started by start.sh. NetworkManager never leaves a working connection for a
# higher-priority one on its own, so after the Cardputer's hotspot has been
# off (Cardputer rebooted, out of range) the UNO Q would stay on the home
# Wi-Fi. Every 60 s: if the "cxl-hotspot" profile exists, isn't active and
# its network shows up in a scan, switch to it. NetworkManager barely scans
# while connected, so ask for a scan when results are older than 30 s
# (--rescan auto) - a short blip once a minute, only while off the hotspot.
while sleep 60; do
  SSID=$(nmcli -g 802-11-wireless.ssid connection show cxl-hotspot 2>/dev/null) || continue
  [ -n "$SSID" ] || continue
  nmcli -t -f NAME connection show --active | grep -qx cxl-hotspot && continue
  if nmcli -t -f SSID dev wifi list --rescan auto 2>/dev/null | grep -qxF "$SSID"; then
    nmcli connection up cxl-hotspot >/dev/null 2>&1
  fi
done
