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

# mosquitto_passwd cree le fichier en 0600 root:root, mais le broker abandonne
# ses privileges pour l'utilisateur mosquitto (uid 1883) et ne peut alors plus
# lire son propre fichier de mots de passe : il redemarre en boucle avec
# "Unable to open pwfile". On corrige le proprietaire depuis un conteneur, pour
# ne pas avoir besoin de sudo sur la machine.
run chown 1883:1883 /mosquitto/config/passwd
run chmod 0640 /mosquitto/config/passwd

echo "OK : comptes esp32, ia, api, dashboard, admin créés (mots de passe chiffrés dans $PASSWD)."
