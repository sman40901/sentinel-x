// =========================================================
// Template — copy this file to "secrets.h" in this same folder, then fill it in.
//
//     cp include/secrets.example.h include/secrets.h
//
// secrets.h is gitignored and must NEVER be committed.
// Everything that is not a secret lives in config.h instead.
// =========================================================
#pragma once

// ---------------------------------------------------------
// 1. The network the BOARD JOINS (to reach the MQTT broker)
//
// The server PC's hotspot - the network MQTT_HOST in config.h is on.
//
// Must be WPA2-PSK. WiFi.begin() cannot join WPA2-Enterprise (802.1X/PEAP)
// networks, which is what most campus WiFi is.
//
// The board hosts no access point and no web page of its own: it is watched
// and driven from the main dashboard, over MQTT.
// ---------------------------------------------------------
#define WIFI_SSID       "SENTINELX-G02"
#define WIFI_PASS       "change-me-wifi"

// ---------------------------------------------------------
// 2. MQTT password for the "esp32" account
//
// Must equal MQTT_ESP32_PASSWORD in the server's .env file.
// ---------------------------------------------------------
#define MQTT_PASSWORD   "change-me-esp32"

// ---------------------------------------------------------
// 3. Maintenance PIN
//
// Typed on the dashboard to switch the box to maintenance (alarms silent,
// controls and self-test unlocked). It never crosses the network: the
// dashboard proves it knows the PIN by challenge-response (see config.h,
// section 7).
//
// While this is still "change-me", or shorter than 4 characters, the board
// REFUSES remote maintenance outright. Use 6+ digits: with the lockout after
// 5 wrong tries, a 6-digit PIN takes years to guess online.
//
// Wrapped in #ifndef so a test build can override it with -DMAINT_PIN=...
// ---------------------------------------------------------
#ifndef MAINT_PIN
  #define MAINT_PIN     "change-me"
#endif

// ---------------------------------------------------------
// 4. Server CA certificate — only needed when MQTT_USE_TLS is 1
//
// Paste the ENTIRE contents of the server's certs/ca.crt between the
// R"EOF( and )EOF" markers, including both BEGIN/END lines.
//
// The board pins this CA with setTrustAnchors(), so a wrong or truncated
// paste shows up as MQTT state -2 plus a TLS error on serial.
//
// Deliberately NOT marked PROGMEM: BearSSL::X509List walks this buffer with
// plain pointer arithmetic at construction time.
// ---------------------------------------------------------
static const char CA_CERT[] = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE THE CONTENTS OF certs/ca.crt HERE
-----END CERTIFICATE-----
)EOF";
