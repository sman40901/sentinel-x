#ifndef CONFIG_H
#define CONFIG_H

// =========================================================
// Secrets (Wi-Fi, mot de passe MQTT, certificat CA)
// -> dans include/secrets.h, ignoré par Git (voir secrets.example.h)
// =========================================================
#include "secrets.h"

// =========================================================
// Configuration de l'environnement
// =========================================================

// 1 = production (TLS 8883 + vérification du certificat) — OBLIGATOIRE avec le serveur Docker
// 0 = test sur un broker local sans TLS (port 1883)
#ifndef IS_PRODUCTION
#define IS_PRODUCTION 1
#endif

// =========================================================
// Configuration MQTT
// =========================================================

#define MQTT_HOST "192.168.137.1"   // IP du PC serveur sur le point d'accès Windows
#define GROUP_ID "g02"              // doit correspondre à l'ACL Mosquitto
#define MQTT_USERNAME "esp32"

#if IS_PRODUCTION
  #define MQTT_PORT 8883
  #define MQTT_USE_TLS true
#else
  #define MQTT_PORT 1883
  #define MQTT_USE_TLS false
#endif

// =========================================================
// Affectation des broches GPIO
// =========================================================

#define PIN_DHT22 4           // Broche de données du DHT22
#define PIN_MQ2_AO 34         // Sortie analogique du MQ-2 via pont diviseur (GPIO 32-39)
#define PIN_PIR 27            // Capteur de mouvement PIR
#define PIN_BUZZER 26         // Buzzer actif
#define PIN_LED_RED 25        // LED rouge
#define PIN_LED_YELLOW 33     // LED jaune

// =========================================================
// Configuration des intervalles de temps
// =========================================================

#define TELEMETRY_INTERVAL 5000        // 5 secondes (ms)
#define MQTT_RECONNECT_INTERVAL 5000   // 5 secondes (ms)
#define GAS_ALERT_COOLDOWN 30000       // au plus une alerte gaz toutes les 30 s
#define PIR_ALERT_COOLDOWN 10000       // au plus une alerte mouvement toutes les 10 s
#define MQ2_WARMUP_MS 60000            // le MQ-2 chauffe ~1 min : pas d'alerte gaz avant

// =========================================================
// Seuils
// =========================================================

// Alarme LOCALE de secours uniquement : la détection d'anomalies est faite par l'IA
#define GAS_ALERT_THRESHOLD 1500

#endif // CONFIG_H
