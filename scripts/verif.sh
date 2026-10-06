#!/usr/bin/env bash
# Vérifie la configuration des secrets Sentinel-X (n'affiche aucun secret)
# Usage : bash scripts/verif.sh
cd "$(dirname "$0")/.." || exit 1
export MSYS_NO_PATHCONV=1

SECRETS=firmware/include/secrets.h
EXAMPLE=firmware/include/secrets.example.h

ok() { echo "OK  $1"; }
ko() { echo "KO  $1"; }
check() { if [ "$2" = 0 ]; then ok "$1"; else ko "$1"; fi; }

env_val() {
  [ -f .env ] || return 0
  grep -m1 "^$1=" .env | cut -d= -f2- | tr -d '\r' | sed -E "s/^\"(.*)\"\$/\1/; s/^'(.*)'\$/\1/"
}
ENV_PWD=$(env_val MQTT_ESP32_PASSWORD)

# a) secrets.h existe
[ -f "$SECRETS" ]; check "a) $SECRETS existe" $?

# b) MQTT_PASSWORD de secrets.h == MQTT_ESP32_PASSWORD de .env
H_PWD=""
[ -f "$SECRETS" ] && H_PWD=$(tr -d '\r' < "$SECRETS" | sed -nE 's/^#define[[:space:]]+MQTT_PASSWORD[[:space:]]+"([^"]*)".*/\1/p' | head -1)
[ -n "$ENV_PWD" ] && [ "$H_PWD" = "$ENV_PWD" ]; check "b) MQTT_PASSWORD (secrets.h) == MQTT_ESP32_PASSWORD (.env)" $?

# c) bloc certificat de secrets.h == certs/ca.crt (sans espaces ni retours ligne)
norm_cert() { sed -n '/-----BEGIN CERTIFICATE-----/,/-----END CERTIFICATE-----/p' | tr -d ' \t\r\n'; }
H_CERT=""; CA=""
[ -f "$SECRETS" ] && H_CERT=$(norm_cert < "$SECRETS")
[ -f certs/ca.crt ] && CA=$(norm_cert < certs/ca.crt)
[ -n "$CA" ] && [ "$H_CERT" = "$CA" ]; check "c) CA_CERT (secrets.h) == certs/ca.crt" $?

# d) certs/server.crt contient 192.168.137.1 dans subjectAltName
SAN=""
if [ -f certs/server.crt ]; then
  if command -v openssl >/dev/null 2>&1; then
    SAN=$(openssl x509 -in certs/server.crt -noout -ext subjectAltName 2>/dev/null)
  else
    SAN=$(docker exec -i sentinelx-mosquitto sh -c 'cat > /tmp/s.crt && openssl x509 -in /tmp/s.crt -noout -ext subjectAltName' < certs/server.crt 2>/dev/null)
  fi
fi
echo "$SAN" | grep -q 'IP Address:192\.168\.137\.1\b'; check "d) certs/server.crt : 192.168.137.1 dans subjectAltName" $?

# e) aucun fichier secret suivi par Git
TRACKED=$(git ls-files | grep -E '(^|/)\.env$|(^|/)secrets\.h$|\.key$|(^|/)passwd$')
[ -z "$TRACKED" ]; check "e) aucun fichier secret suivi par Git" $?
[ -n "$TRACKED" ] && echo "$TRACKED" | sed 's/^/      suivi : /'

# f) secrets.example.h contient "change-moi"
grep -q 'change-moi' "$EXAMPLE" 2>/dev/null; check "f) $EXAMPLE contient \"change-moi\"" $?

# g) Mosquitto accepte le compte esp32 avec le mot de passe du .env
if [ -n "$ENV_PWD" ]; then
  OUT=$(docker exec sentinelx-mosquitto mosquitto_sub -h localhost -p 1883 -u esp32 -P "$ENV_PWD" -t 'sentinelx/g02/cmd' -C 1 -W 3 2>&1)
  RC=$?
  if echo "$OUT" | grep -qi 'not authori'; then RC=1
  elif echo "$OUT" | grep -qiE 'no such container|is not running|error during connect|connection refused'; then RC=1
  else RC=0; fi
else
  RC=1
fi
check "g) Mosquitto accepte esp32 avec le mot de passe du .env" $RC
