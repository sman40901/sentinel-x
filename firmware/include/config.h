#ifndef CONFIG_H
#define CONFIG_H

// =========================================================
// WiFi Configuration
// =========================================================

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// =========================================================
// MQTT Configuration
// =========================================================

#define MQTT_HOST "mosquitto"
#define MQTT_PORT 1883
#define MQTT_USERNAME "api"
#define MQTT_PASSWORD "your_mqtt_password"
#define GROUP_ID "g0X"

// =========================================================
// GPIO Pin Assignments
// =========================================================

#define PIN_DHT22 4           // DHT22 data pin
#define PIN_MQ2_AO 34         // MQ-2 analog output (GPIO 32-39)
#define PIN_PIR 23            // PIR motion sensor
#define PIN_BUZZER 22         // Active buzzer
#define PIN_LED_RED 19        // Red LED
#define PIN_LED_YELLOW 18     // Yellow LED

// =========================================================
// Timing Configuration
// =========================================================

#define TELEMETRY_INTERVAL 2000    // 2 seconds (ms)
#define MQTT_RECONNECT_INTERVAL 5000  // 5 seconds (ms)

// =========================================================
// Thresholds
// =========================================================

#define GAS_ALERT_THRESHOLD 400    // Analog value threshold for gas alert

#endif // CONFIG_H
