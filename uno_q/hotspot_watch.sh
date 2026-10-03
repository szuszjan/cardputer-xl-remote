#!/bin/bash
# Started by start.sh. NetworkManager never leaves a working connection for a
# higher-priority one on its own, so after the Cardputer's hotspot has been
# off (Cardputer rebooted, out of range) the UNO Q would stay on the home
# Wi-Fi. Every 30 s: if the "cxl-hotspot" profile exists, isn't active and
# its network is in the latest scan results, switch to it.
# Uses NetworkManager's own periodic scans (--rescan no), so this never
# interrupts the current connection to scan.
while sleep 30; do
  SSID=$(nmcli -g 802-11-wireless.ssid connection show cxl-hotspot 2>/dev/null) || continue
  [ -n "$SSID" ] || continue
  nmcli -t -f NAME connection show --active | grep -qx cxl-hotspot && continue
  if nmcli -t -f SSID dev wifi list --rescan no 2>/dev/null | grep -qxF "$SSID"; then
    nmcli connection up cxl-hotspot >/dev/null 2>&1
  fi
done
