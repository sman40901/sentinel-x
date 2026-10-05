#!/usr/bin/env bash
# Crée les 5 comptes MQTT (esp32, ia, api, dashboard, admin) avec les mots de passe du fichier .env
# Usage : ./scripts/add-mqtt-users.sh
set -euo pipefail
cd "$(dirname "$0")/.."

[ -f .env ] || { echo "Fichier .env manquant : cp .env.example .env puis remplis-le."; exit 1; }
set -a; . ./.env; set +a

PASSWD=mosquitto/config/passwd
: "${MQTT_DASHBOARD_PASSWORD:?Ajoute MQTT_DASHBOARD_PASSWORD dans .env}"
rm -f "$PASSWD"

run() {
  docker run --rm -v "$(pwd)/mosquitto/config:/mosquitto/config" eclipse-mosquitto:2 "$@"
}

run mosquitto_passwd -c -b /mosquitto/config/passwd esp32 "$MQTT_ESP32_PASSWORD"
run mosquitto_passwd    -b /mosquitto/config/passwd ia    "$MQTT_IA_PASSWORD"
run mosquitto_passwd    -b /mosquitto/config/passwd api   "$MQTT_API_PASSWORD"
run mosquitto_passwd    -b /mosquitto/config/passwd dashboard "$MQTT_DASHBOARD_PASSWORD"
run mosquitto_passwd    -b /mosquitto/config/passwd admin "$MQTT_ADMIN_PASSWORD"

echo "OK : comptes esp32, ia, api, dashboard, admin créés (mots de passe chiffrés dans $PASSWD)."
