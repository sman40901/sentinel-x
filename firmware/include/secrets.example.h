// =========================================================
// Template — copy this file to "secrets.h" in this same folder, then fill it in.
//
//     cp include/secrets.example.h include/secrets.h
//
// secrets.h is gitignored and must NEVER be committed.
// Everything that is not a password lives in config.h instead.
// =========================================================
#pragma once

// ---------------------------------------------------------
// 1. The access point the BOARD HOSTS (dashboard lives here)
//
// Connect your phone or laptop to this, then open http://192.168.4.1/
// Password must be at least 8 characters, or the AP silently opens unsecured.
// ---------------------------------------------------------
#define AP_SSID         "Sentinel-X"
#define AP_PASS         "change-me-ap"

// ---------------------------------------------------------
// 2. The network the BOARD JOINS (to reach the MQTT broker)
//
// This is the server PC's hotspot — the same SSID as MQTT_HOST's network
// in config.h. Only used when WIFI_JOIN_STATION is 1.
//
// Must be WPA2-PSK. WiFi.begin() cannot join WPA2-Enterprise
// (802.1X/PEAP) networks, which is what most campus WiFi is.
// ---------------------------------------------------------
#define WIFI_SSID       "SENTINELX-G02"
#define WIFI_PASS       "change-me-wifi"

// ---------------------------------------------------------
// 3. MQTT password for the "esp32" account
//
// Must equal MQTT_ESP32_PASSWORD in the server's .env file.
// The username itself is MQTT_USERNAME in config.h.
// ---------------------------------------------------------
#define MQTT_PASSWORD   "change-me-esp32"

// ---------------------------------------------------------
// 4. Server CA certificate — only needed when MQTT_USE_TLS is 1
//
// Paste the ENTIRE contents of the server's certs/ca.crt between the
// R"EOF( and )EOF" markers, including both BEGIN/END lines.
//
// The board pins this CA with setTrustAnchors(), so a wrong or truncated
// paste shows up as MQTT state -2 plus a TLS error on serial.
// ---------------------------------------------------------
// Deliberately NOT marked PROGMEM: BearSSL::X509List walks this buffer with
// plain pointer arithmetic at construction time. It costs ~1.5 kB of RAM, which
// is the price of not debugging a flash-access fault during a workshop.
static const char CA_CERT[] = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE THE CONTENTS OF certs/ca.crt HERE
-----END CERTIFICATE-----
)EOF";
