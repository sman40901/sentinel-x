#ifndef CONFIG_H
#define CONFIG_H

// =========================================================
// Configuration WiFi
// =========================================================

#define WIFI_SSID "your_wifi"
#define WIFI_PASSWORD "your_wifi_password"

// =========================================================
// Configuration de l'environnement
// =========================================================

// Définir à 1 pour la production, 0 pour le test/développement
// Peut être remplacé dans platformio.ini avec build_flags
#ifndef IS_PRODUCTION
#define IS_PRODUCTION 0
#endif

// =========================================================
// Configuration MQTT
// =========================================================

#define MQTT_HOST "192.168.1.50"
#define GROUP_ID "g01"

// Configuration conditionnelle selon l'environnement
#if IS_PRODUCTION
  // Production : TLS, authentification, port 8883
  #define MQTT_PORT 8883
  #define MQTT_USE_TLS true
  #define MQTT_USERNAME "esp32"
  #define MQTT_PASSWORD "your_mqtt_password"
#else
  // Test/Développement : sans TLS, sans authentification, port 1883
  #define MQTT_PORT 1883
  #define MQTT_USE_TLS false
  #define MQTT_USERNAME "esp32"
  #define MQTT_PASSWORD "your_mqtt_password"
#endif

// =========================================================
// Affectation des broches GPIO
// =========================================================

#define PIN_DHT22 4           // Broche de données du DHT22
#define PIN_MQ2_AO 34         // Sortie analogique du MQ-2 (GPIO 32-39)
#define PIN_PIR 27            // Capteur de mouvement PIR
#define PIN_BUZZER 26         // Buzzer actif
#define PIN_LED_RED 25        // LED rouge
#define PIN_LED_YELLOW 33     // LED jaune

// =========================================================
// Configuration des intervalles de temps
// =========================================================

#define TELEMETRY_INTERVAL 5000        // 5 secondes (ms)
#define MQTT_RECONNECT_INTERVAL 5000   // 5 secondes (ms)

// =========================================================
// Seuils
// =========================================================

#define GAS_ALERT_THRESHOLD 1500    // Seuil de la valeur analogique pour l'alerte de gaz

#endif // CONFIG_H