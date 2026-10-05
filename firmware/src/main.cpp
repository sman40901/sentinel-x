#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <WiFiClientSecure.h>
#include <cstring>
#include "config.h"

// =========================================================
// MQTT Topics
// =========================================================

String topicTelemetry = "sentinelx/" + String(GROUP_ID) + "/telemetry";
String topicAlerts = "sentinelx/" + String(GROUP_ID) + "/alerts";
String topicStatus = "sentinelx/" + String(GROUP_ID) + "/status";
String topicCommand = "sentinelx/" + String(GROUP_ID) + "/cmd";

// =========================================================
// Global Objects
// =========================================================

#if MQTT_USE_TLS
WiFiClientSecure wifiClient;
#else
WiFiClient wifiClient;
#endif

PubSubClient mqttClient(wifiClient);
DHT dht(PIN_DHT22, DHT22);

unsigned long lastTelemetryTime = 0;
unsigned long lastReconnectTime = 0;

// =========================================================
// WiFi Connection
// =========================================================

void connectWiFi() {
  Serial.println("Connecting to WiFi...");
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

// =========================================================
// MQTT Callback
// =========================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("]: ");
  
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);

  // Parse command: {"buzzer": 1, "led": "rouge"}
  if (String(topic) == topicCommand) {
    // Parse JSON manually (simple approach)
    int buzzerState = 0;
    String ledColor = "";

    // Extract buzzer value
    int buzzerIndex = message.indexOf("\"buzzer\":");
    if (buzzerIndex != -1) {
      buzzerState = message.substring(buzzerIndex + 9).toInt();
    }

    // Extract led value
    int ledIndex = message.indexOf("\"led\":");
    if (ledIndex != -1) {
      int startQuote = message.indexOf("\"", ledIndex + 6) + 1;
      int endQuote = message.indexOf("\"", startQuote);
      ledColor = message.substring(startQuote, endQuote);
    }

    // Control buzzer
    digitalWrite(PIN_BUZZER, buzzerState ? HIGH : LOW);

    // Control LEDs
    digitalWrite(PIN_LED_RED, LOW);
    digitalWrite(PIN_LED_YELLOW, LOW);

    if (ledColor == "rouge") {
      digitalWrite(PIN_LED_RED, HIGH);
    } else if (ledColor == "jaune") {
      digitalWrite(PIN_LED_YELLOW, HIGH);
    } else if (ledColor == "blanc") {
      digitalWrite(PIN_LED_RED, HIGH);
      digitalWrite(PIN_LED_YELLOW, HIGH);
    }
    // "eteint" keeps both LOW
  }
}

// =========================================================
// MQTT Connection
// =========================================================

void connectMQTT() {
  if (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");

    String clientId = "sentinelx-" + String(GROUP_ID) + "-esp32";

#if MQTT_USE_TLS
    // Configure TLS for production
    wifiClient.setInsecure(); // For development with self-signed certs
    // For production, load CA certificate:
    // wifiClient.setCACert(ca_cert);
    Serial.println(" (TLS enabled)");
#endif

    if (mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
      Serial.println("connected");

      // Subscribe to command topic
      mqttClient.subscribe(topicCommand.c_str());
      Serial.print("Subscribed to: ");
      Serial.println(topicCommand);

      // Publish online status
      mqttClient.publish(topicStatus.c_str(), "online");

    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" retrying in 5 seconds");
    }
  }
}

// =========================================================
// Publish Telemetry
// =========================================================

void publishTelemetry() {
  // Read DHT22
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  // Read MQ-2 (analog)
  int gasValue = analogRead(PIN_MQ2_AO);

  // Read PIR (digital)
  int motionDetected = digitalRead(PIN_PIR);

  // Check for sensor errors
  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("Failed to read from DHT sensor!");
    return;
  }

  // Create JSON payload
  String payload = "{\"t\":" + String(temperature) + 
                   ",\"h\":" + String(humidity) + 
                   ",\"gaz\":" + String(gasValue) + 
                   ",\"pir\":" + String(motionDetected) + "}";

  // Publish telemetry
  if (mqttClient.publish(topicTelemetry.c_str(), payload.c_str())) {
    Serial.print("Telemetry published: ");
    Serial.println(payload);
  } else {
    Serial.println("Failed to publish telemetry");
  }

  // Check for gas alert
  if (gasValue > GAS_ALERT_THRESHOLD) {
    String alertPayload = "{\"type\":\"gaz\",\"niveau\":\"critique\"}";
    mqttClient.publish(topicAlerts.c_str(), alertPayload.c_str());
    Serial.println("Gas alert published!");
  }
}

// =========================================================
// Setup
// =========================================================

void setup() {
  Serial.begin(115200);
  Serial.println("Sentinel-X ESP32 Firmware");
  Serial.print("Environment: ");
  Serial.println(IS_PRODUCTION ? "PRODUCTION" : "TEST/DEVELOPMENT");
  Serial.print("MQTT Port: ");
  Serial.println(MQTT_PORT);
  Serial.print("MQTT TLS: ");
  Serial.println(MQTT_USE_TLS ? "enabled" : "disabled");
  Serial.print("MQTT Auth: ");
  Serial.println(strlen(MQTT_USERNAME) > 0 ? "enabled" : "disabled");

  // Initialize pins
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_PIR, INPUT);

  // Initialize sensors
  dht.begin();

  // Connect to WiFi
  connectWiFi();

  // Configure MQTT
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  // Connect to MQTT
  connectMQTT();
}

// =========================================================
// Main Loop
// =========================================================

void loop() {
  unsigned long currentTime = millis();

  // Handle MQTT connection
  if (currentTime - lastReconnectTime >= MQTT_RECONNECT_INTERVAL) {
    lastReconnectTime = currentTime;
    if (!mqttClient.connected()) {
      connectMQTT();
    }
  }

  // MQTT loop
  if (mqttClient.connected()) {
    mqttClient.loop();

    // Publish telemetry periodically
    if (currentTime - lastTelemetryTime >= TELEMETRY_INTERVAL) {
      lastTelemetryTime = currentTime;
      publishTelemetry();
    }
  }

  // Small delay to prevent watchdog issues
  delay(10);
}
