#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <WiFiClientSecure.h>
#include <cstring>
#include "config.h"

// =========================================================
// Topics MQTT
// =========================================================

String topicTelemetry = "sentinelx/" + String(GROUP_ID) + "/telemetry";
String topicAlerts = "sentinelx/" + String(GROUP_ID) + "/alerts";
String topicStatus = "sentinelx/" + String(GROUP_ID) + "/status";
String topicCommand = "sentinelx/" + String(GROUP_ID) + "/cmd";

// =========================================================
// Objets globaux
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
// Connexion WiFi
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
// Fonction de rappel MQTT
// =========================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("\]: ");
  
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);

  // Analyser la commande : {"buzzer": 1, "led": "rouge"}
  if (String(topic) == topicCommand) {
    // Analyser le JSON manuellement (approche simple)
    int buzzerState = 0;
    String ledColor = "";

    // Extraire la valeur du buzzer
    int buzzerIndex = message.indexOf("\"buzzer\":");
    if (buzzerIndex != -1) {
      buzzerState = message.substring(buzzerIndex + 9).toInt();
    }

    // Extraire la valeur de la LED
    int ledIndex = message.indexOf("\"led\":");
    if (ledIndex != -1) {
      int startQuote = message.indexOf("\"", ledIndex + 6) + 1;
      int endQuote = message.indexOf("\"", startQuote);
      ledColor = message.substring(startQuote, endQuote);
    }

    // Contrôler le buzzer
    digitalWrite(PIN_BUZZER, buzzerState ? HIGH : LOW);

    // Contrôler les LED
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
    // "eteint" maintient les deux LED éteintes (LOW)
  }
}

// =========================================================
// Connexion MQTT
// =========================================================

void connectMQTT() {
  if (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");

    String clientId = "sentinelx-" + String(GROUP_ID) + "-esp32";

#if MQTT_USE_TLS
    // Configurer TLS pour la production
    wifiClient.setInsecure(); // Pour le développement avec des certificats auto-signés
    // Pour la production, charger le certificat CA :
    // wifiClient.setCACert(ca_cert);
    Serial.println(" (TLS enabled)");
#endif

    if (mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
      Serial.println("connected");

      // S'abonner au topic des commandes
      mqttClient.subscribe(topicCommand.c_str());
      Serial.print("Subscribed to: ");
      Serial.println(topicCommand);

      // Publier le statut "online"
      mqttClient.publish(topicStatus.c_str(), "online");

    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" retrying in 5 seconds");
    }
  }
}

// =========================================================
// Publication de la télémétrie
// =========================================================

void publishTelemetry() {
  // Lire le DHT22
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  // Lire le MQ-2 (analogique)
  int gasValue = analogRead(PIN_MQ2_AO);

  // Lire le PIR (numérique)
  int motionDetected = digitalRead(PIN_PIR);

  // Vérifier les erreurs des capteurs
  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("Failed to read from DHT sensor!");
    return;
  }

  // Créer la charge utile JSON
  String payload = "{\"t\":" + String(temperature) + 
                   ",\"h\":" + String(humidity) + 
                   ",\"gaz\":" + String(gasValue) + 
                   ",\"pir\":" + String(motionDetected) + "}";

  // Publier la télémétrie
  if (mqttClient.publish(topicTelemetry.c_str(), payload.c_str())) {
    Serial.print("Telemetry published: ");
    Serial.println(payload);
  } else {
    Serial.println("Failed to publish telemetry");
  }

  // Vérifier si le seuil d'alerte de gaz est dépassé
  if (gasValue > GAS_ALERT_THRESHOLD) {
    String alertPayload = "{\"type\":\"gaz\",\"niveau\":\"critique\"}";
    mqttClient.publish(topicAlerts.c_str(), alertPayload.c_str());
    Serial.println("Gas alert published!");
  
}
}

// =========================================================
// Initialisation
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

  // Initialiser les broches
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_PIR, INPUT);

  // Initialiser les capteurs
  dht.begin();

  // Se connecter au WiFi
  connectWiFi();

  // Configurer MQTT
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  // Se connecter à MQTT
  connectMQTT();
}

// =========================================================
// Boucle principale
// =========================================================

void loop() {
  unsigned long currentTime = millis();

  // Gérer la connexion MQTT
  if (currentTime - lastReconnectTime >= MQTT_RECONNECT_INTERVAL) {
    lastReconnectTime = currentTime;
    if (!mqttClient.connected()) {
      connectMQTT();
    }
  }

  // Boucle MQTT
  if (mqttClient.connected()) {
    mqttClient.loop();

    // Publier périodiquement la télémétrie
    if (currentTime - lastTelemetryTime >= TELEMETRY_INTERVAL) {
      lastTelemetryTime = currentTime;
      publishTelemetry();
    }
  }

  // Petit délai pour éviter les problèmes liés au watchdog
  delay(10);
}