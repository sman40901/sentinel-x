#!/usr/bin/env bash
# Cree nginx/htpasswd : l'authentification HTTP de la camera, des photos et de l'API.
#
# Pourquoi : un audit a montre que le flux video en direct, les 33 photos
# d'evenements et GET /api/v1/mesures repondaient tous 200 sans le moindre
# identifiant. Quiconque joignait le reseau pouvait regarder la camera.
#
# Par defaut le mot de passe est celui du compte MQTT 'dashboard', pour n'avoir
# qu'un seul secret a retenir. Mets DASHBOARD_HTTP_PASSWORD dans .env pour
# les separer.
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f .env ] && set -a && . ./.env && set +a

USER_="${DASHBOARD_HTTP_USER:-dashboard}"
PASS="${DASHBOARD_HTTP_PASSWORD:-${MQTT_DASHBOARD_PASSWORD:-}}"
if [ -z "$PASS" ]; then
  echo "Ajoute DASHBOARD_HTTP_PASSWORD (ou MQTT_DASHBOARD_PASSWORD) dans .env" >&2
  exit 1
fi

# -apr1 : le seul format que l'openssl de base sait produire et que nginx sait
# lire. apache2-utils (htpasswd) n'est pas garanti present sur la machine.
printf '%s:%s\n' "$USER_" "$(openssl passwd -apr1 "$PASS")" > nginx/htpasswd
chmod 644 nginx/htpasswd
echo "nginx/htpasswd ecrit  (utilisateur : $USER_)"
