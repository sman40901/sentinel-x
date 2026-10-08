#!/usr/bin/env bash
# Bascule le PC en point d'acces pour le boitier.
#
# ATTENTION : cette machine n'a qu'une seule radio WiFi. Pendant que le point
# d'acces tourne, il n'y a PAS d'acces Internet. Utiliser scripts/hotspot-off.sh
# pour revenir.
#
# L'adresse 10.42.0.1 est epinglee dans le profil et inscrite dans le
# certificat du serveur : ne pas la changer sans regenerer les certificats.
set -euo pipefail

WIFI="${WIFI_CONNECTION:-COMDEV-PEDAGO}"

echo "Coupure de $WIFI et activation du point d'acces..."
# L'autoconnect de la WiFi normale doit etre desactive, sinon NetworkManager
# reprend la radio au bout de quelques secondes et le point d'acces meurt.
nmcli con modify "$WIFI" connection.autoconnect no 2>/dev/null || true
nmcli con down "$WIFI" 2>/dev/null || true
nmcli con up Hotspot

sleep 3
ADDR=$(ip -4 -br addr show wlp1s0 | awk '{print $3}')
echo
echo "  wlp1s0 : $ADDR"
if [ "$ADDR" = "10.42.0.1/24" ]; then
  echo "  OK - le boitier peut joindre le broker."
  echo
  echo "  Dashboard : https://10.42.0.1/"
  echo "  Retour WiFi : bash scripts/hotspot-off.sh"
else
  echo "  ECHEC : adresse inattendue. Le point d'acces n'a pas pris la radio."
  echo "  Verifier : journalctl -u NetworkManager -n 30"
  exit 1
fi
