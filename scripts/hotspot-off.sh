#!/usr/bin/env bash
# Rend la radio au WiFi normal et retablit l'acces Internet.
set -euo pipefail

WIFI="${WIFI_CONNECTION:-COMDEV-PEDAGO}"

echo "Arret du point d'acces, retour sur $WIFI..."
nmcli con down Hotspot 2>/dev/null || true
nmcli con modify "$WIFI" connection.autoconnect yes
nmcli con up "$WIFI"

sleep 3
echo
echo "  wlp1s0 : $(ip -4 -br addr show wlp1s0 | awk '{print $3}')"
echo "  Internet retabli."
