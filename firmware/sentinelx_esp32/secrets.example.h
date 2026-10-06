// =========================================================
// Copier ce fichier en "secrets.h" (même dossier include/) puis le remplir.
// secrets.h ne doit JAMAIS être commité (il est dans .gitignore).
// =========================================================
#pragma once

#define WIFI_SSID     "SENTINELX-G02"
#define WIFI_PASSWORD "change-moi-wifi"

#define MQTT_PASSWORD "change-moi-esp32"   // = MQTT_ESP32_PASSWORD du .env serveur

// Contenu complet de certs/ca.crt du serveur
static const char CA_CERT[] = R"EOF(
-----BEGIN CERTIFICATE-----
COLLER ICI LE CONTENU DE certs/ca.crt
-----END CERTIFICATE-----
)EOF";
