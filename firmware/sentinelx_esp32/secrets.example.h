// ============================================================================
//  Sentinel-X — secrets du boîtier
//  1. Copier ce fichier en "secrets.h" (même dossier)
//  2. Remplir les valeurs
//  secrets.h est dans .gitignore : il ne doit JAMAIS être commité.
// ============================================================================
#pragma once

// --- Wi-Fi de la table (point d'accès du PC serveur, en 2,4 GHz) ---
#define WIFI_SSID      "SENTINELX-G02"
#define WIFI_PASSWORD  "change-moi-wifi"

// --- Broker MQTT (PC serveur) ---
#define MQTT_HOST      "192.168.137.1"     // IP du PC sur le point d'accès Windows
#define MQTT_PORT      8883                // MQTTS (chiffré)
#define MQTT_USER      "esp32"
#define MQTT_PASSWORD  "change-moi-esp32"  // = MQTT_ESP32_PASSWORD du .env

// --- Certificat de l'autorité (contenu complet du fichier certs/ca.crt) ---
static const char CA_CERT[] = R"EOF(
-----BEGIN CERTIFICATE-----
COLLER ICI LE CONTENU DE certs/ca.crt
-----END CERTIFICATE-----
)EOF";
