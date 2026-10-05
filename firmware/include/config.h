#ifndef CONFIG_H
#define CONFIG_H

// =========================================================
// WiFi Configuration
// =========================================================

#define WIFI_SSID "your_wifi"
#define WIFI_PASSWORD "your_wifi_password"

// =========================================================
// Environment Configuration
// =========================================================

// Set to 1 for production, 0 for test/development
// Can be overridden in platformio.ini with build_flags
#ifndef IS_PRODUCTION
#define IS_PRODUCTION 0
#endif

// =========================================================
// MQTT Configuration
// =========================================================

#define MQTT_HOST "192.168.1.50"
#define GROUP_ID "g01"

// Conditional configuration based on environment
#if IS_PRODUCTION
  // Production: TLS, authentication, port 8883
  #define MQTT_PORT 8883
  #define MQTT_USE_TLS true
  #define MQTT_USERNAME "esp32"
  #define MQTT_PASSWORD "your_mqtt_password"
#else
  // Test/Development: No TLS, no auth, port 1883
  #define MQTT_PORT 1883
  #define MQTT_USE_TLS false
  #define MQTT_USERNAME "esp32"
  #define MQTT_PASSWORD "your_mqtt_password"
#endif

// =========================================================
// GPIO Pin Assignments
// =========================================================

#define PIN_DHT22 4           // DHT22 data pin
#define PIN_MQ2_AO 34         // MQ-2 analog output (GPIO 32-39)
#define PIN_PIR 27            // PIR motion sensor
#define PIN_BUZZER 26         // Active buzzer
#define PIN_LED_RED 25        // Red LED
#define PIN_LED_YELLOW 33     // Yellow LED

// =========================================================
// Timing Configuration
// =========================================================

#define TELEMETRY_INTERVAL 5000    // 5 seconds (ms)
#define MQTT_RECONNECT_INTERVAL 5000  // 5 seconds (ms)

// =========================================================
// Thresholds
// =========================================================

#define GAS_ALERT_THRESHOLD 1500    // Analog value threshold for gas alert

#endif // CONFIG_H