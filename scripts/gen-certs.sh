#!/usr/bin/env bash
# Génère une autorité de certification (CA) et le certificat du serveur
# Usage : bash scripts/gen-certs.sh
# IP du serveur incluses par défaut : 192.168.137.1 (point d'accès Windows) et 192.168.10.1
#   autre IP : SERVER_IPS="192.168.x.y" bash scripts/gen-certs.sh
set -euo pipefail
cd "$(dirname "$0")/../certs"

SERVER_IPS="${SERVER_IPS:-192.168.137.1 192.168.10.1}"
FIRST_IP="${SERVER_IPS%% *}"
# Chaque adresse est inscrite DEUX FOIS : en IP: et en DNS:.
#
# Ce n'est pas une redondance. Le moteur X.509 "minimal" de BearSSL, celui de
# l'ESP8266, ne compare le nom attendu qu'aux entrees dNSName : il ignore
# totalement les SAN de type iPAddress. Une connexion a "10.42.0.1" echoue donc
# avec "Expected server name was not found in the chain" meme quand IP:10.42.0.1
# est bien dans le certificat. Les navigateurs, eux, utilisent l'entree IP:.
SAN=""
for ip in $SERVER_IPS; do SAN="${SAN}IP:${ip}, DNS:${ip}, "; done

if [ -f ca.key ]; then
  echo "Des certificats existent déjà dans certs/. Pour regénérer : rm certs/*.crt certs/*.key"
  exit 1
fi

# 1. Autorité de certification du groupe
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout ca.key -out ca.crt \
  -subj "/C=FR/O=Sentinel-X/CN=Sentinel-X CA"

# 2. Clé + demande du serveur
openssl req -newkey rsa:2048 -nodes \
  -keyout server.key -out server.csr \
  -subj "/C=FR/O=Sentinel-X/CN=${FIRST_IP}"

# 3. Signature avec les adresses du serveur (SAN), obligatoire pour l'ESP32 et les navigateurs
cat > server.ext <<EOF
subjectAltName = ${SAN}IP:127.0.0.1, DNS:sentinelx.local, DNS:localhost, DNS:mosquitto
extendedKeyUsage = serverAuth
EOF
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt -days 365 -sha256 -extfile server.ext

rm -f server.csr server.ext ca.srl
chmod 600 ca.key
chmod 644 ca.crt server.crt
# Lisible par l'utilisateur mosquitto du conteneur. Pour durcir : sudo chown 1883:1883 server.key && chmod 640 server.key
chmod 644 server.key

echo
echo "OK. Fichiers créés dans certs/ :"
echo "  ca.crt      -> à donner aux DEV (à mettre dans le firmware ESP32)"
echo "  server.crt  -> certificat du serveur (Mosquitto + nginx)"
echo "  server.key  -> clé privée du serveur, ne JAMAIS la commiter"
echo "  ca.key      -> clé de la CA, à garder pour toi"
