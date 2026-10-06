#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <WiFiClientSecure.h>
#include <cstring>
#include <cstdlib>
#include "config.h"

// =========================================================
// Topics MQTT
// =========================================================

const char* topicTelemetry = "sentinelx/" GROUP_ID "/telemetry";
const char* topicAlerts    = "sentinelx/" GROUP_ID "/alerts";
const char* topicStatus    = "sentinelx/" GROUP_ID "/status";
const char* topicCommand   = "sentinelx/" GROUP_ID "/cmd";

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
unsigned long lastGasAlert = 0;
unsigned long lastPirAlert = 0;
bool pirPrevious = false;
bool pirSeen = false;          // mouvement vu depuis la dernière télémétrie

// =========================================================
// Connexion WiFi
// =========================================================

void connectWiFi() {
  Serial.print("Connecting to WiFi ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi not connected yet (2.4 GHz ? SSID ?), retrying in background");
  }
}

// =========================================================
// Lecture simple d'un champ JSON (sans bibliothèque)
// =========================================================

// Renvoie la position juste après "cle": ou nullptr si absente
const char* jsonField(const char* json, const char* key) {
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char* p = strstr(json, pattern);
  if (!p) return nullptr;
  p = strchr(p + strlen(pattern), ':');
  if (!p) return nullptr;
  p++;
  while (*p == ' ') p++;
  return p;
}

// =========================================================
// Fonction de rappel MQTT : {"buzzer": 1} et/ou {"led": "rouge"}
// =========================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char message[128];
  unsigned int n = length < sizeof(message) - 1 ? length : sizeof(message) - 1;
  memcpy(message, payload, n);
  message[n] = '\0';

  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  if (strcmp(topic, topicCommand) != 0) return;

  // Buzzer : seulement si la clé est présente
  const char* b = jsonField(message, "buzzer");
  if (b) {
    digitalWrite(PIN_BUZZER, atoi(b) == 1 ? HIGH : LOW);
  }

  // LEDs : seulement si la clé est présente
  const char* l = jsonField(message, "led");
  if (l && *l == '"') {
    char color[16] = {0};
    const char* end = strchr(l + 1, '"');
    if (end && end - (l + 1) < (int)sizeof(color)) {
      memcpy(color, l + 1, end - (l + 1));
    }
    digitalWrite(PIN_LED_RED, LOW);
    digitalWrite(PIN_LED_YELLOW, LOW);
    if (strcmp(color, "rouge") == 0) {
      digitalWrite(PIN_LED_RED, HIGH);
    } else if (strcmp(color, "jaune") == 0) {
      digitalWrite(PIN_LED_YELLOW, HIGH);
    } else if (strcmp(color, "blanc") == 0) {
      digitalWrite(PIN_LED_RED, HIGH);
      digitalWrite(PIN_LED_YELLOW, HIGH);
    }
    // "off", "eteint", "vert" (pas de LED verte) : tout reste éteint
  }
}

// =========================================================
// Connexion MQTT
// =========================================================

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) return;

  Serial.print("Attempting MQTT connection to ");
  Serial.print(MQTT_HOST);
  Serial.print(":");
  Serial.print(MQTT_PORT);
  Serial.println(MQTT_USE_TLS ? " (TLS)" : " (no TLS)");

  char clientId[32];
  snprintf(clientId, sizeof(clientId), "sentinelx-%s-esp32", GROUP_ID);

  // LWT : si le boîtier disparaît, le broker publie "offline" à sa place
  if (mqttClient.connect(clientId, MQTT_USERNAME, MQTT_PASSWORD, topicStatus, 1, true, "offline")) {
    Serial.println("connected");
    mqttClient.subscribe(topicCommand, 1);
    Serial.print("Subscribed to: ");
    Serial.println(topicCommand);
    mqttClient.publish(topicStatus, "online", true);
  } else {
    Serial.print("failed, rc=");
    Serial.print(mqttClient.state());
#if MQTT_USE_TLS
    char tlsError[100] = {0};
    wifiClient.lastError(tlsError, sizeof(tlsError));
    Serial.print("  TLS: ");
    Serial.print(tlsError);
#endif
    Serial.println("  retrying in 5 seconds");
    // rc=-2 : réseau/TLS (IP, port 8883, pare-feu, certificat)   rc=4/5 : identifiants ou ACL
  }
}

// =========================================================
// Alertes
// =========================================================

void publishAlert(const char* type, const char* level, const char* msg) {
  char alert[160];
  snprintf(alert, sizeof(alert),
           "{\"type\":\"%s\",\"niveau\":\"%s\",\"msg\":\"%s\",\"source\":\"esp32\"}",
           type, level, msg);
  mqttClient.publish(topicAlerts, alert);
  Serial.print("Alert published: ");
  Serial.println(alert);
}

// =========================================================
// Publication de la télémétrie
// =========================================================

void publishTelemetry() {
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  // MQ-2 : moyenne de 8 lectures pour lisser le bruit
  long sum = 0;
  for (int i = 0; i < 8; i++) { sum += analogRead(PIN_MQ2_AO); delay(2); }
  int gasValue = (int)(sum / 8);

  int motion = pirSeen ? 1 : 0;
  pirSeen = false;

  // Une lecture DHT ratée n'empêche pas d'envoyer gaz et mouvement
  char t[12], h[12];
  if (isnan(temperature)) strcpy(t, "null"); else snprintf(t, sizeof(t), "%.1f", temperature);
  if (isnan(humidity))    strcpy(h, "null"); else snprintf(h, sizeof(h), "%.0f", humidity);
  if (isnan(temperature) || isnan(humidity)) Serial.println("Failed to read from DHT sensor!");

  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"t\":%s,\"h\":%s,\"gaz\":%d,\"pir\":%d,\"rssi\":%d}",
           t, h, gasValue, motion, (int)WiFi.RSSI());

  if (mqttClient.publish(topicTelemetry, payload)) {
    Serial.print("Telemetry published: ");
    Serial.println(payload);
  } else {
    Serial.println("Failed to publish telemetry");
  }

  // Alarme locale de secours (la détection d'anomalies est faite par l'IA)
  bool warmedUp = millis() > MQ2_WARMUP_MS;
  if (warmedUp && gasValue > GAS_ALERT_THRESHOLD && millis() - lastGasAlert > GAS_ALERT_COOLDOWN) {
    lastGasAlert = millis();
    publishAlert("gaz", "critique", "Seuil gaz MQ-2 depasse");
  }
}

// =========================================================
// Initialisation
// =========================================================

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("Sentinel-X ESP32 Firmware");
  Serial.print("Environment: ");
  Serial.println(IS_PRODUCTION ? "PRODUCTION" : "TEST/DEVELOPMENT");

  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_PIR, INPUT);
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_YELLOW, LOW);

  dht.begin();

  connectWiFi();

#if MQTT_USE_TLS
  wifiClient.setCACert(CA_CERT);   // vérifie que le serveur est bien le nôtre
#endif
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(10);

  connectMQTT();
}

// =========================================================
// Boucle principale
// =========================================================

void loop() {
  unsigned long currentTime = millis();

  // PIR lu en continu (un passage bref n'est plus manqué)
  bool pir = digitalRead(PIN_PIR) == HIGH;
  if (pir) pirSeen = true;
  if (pir && !pirPrevious && mqttClient.connected() && currentTime - lastPirAlert > PIR_ALERT_COOLDOWN) {
    lastPirAlert = currentTime;
    publishAlert("mouvement", "attention", "Mouvement detecte par le PIR");
  }
  pirPrevious = pir;

  // Reconnexion MQTT
  if (currentTime - lastReconnectTime >= MQTT_RECONNECT_INTERVAL) {
    lastReconnectTime = currentTime;
    connectMQTT();
  }

  if (mqttClient.connected()) {
    mqttClient.loop();

    if (currentTime - lastTelemetryTime >= TELEMETRY_INTERVAL) {
      lastTelemetryTime = currentTime;
      publishTelemetry();
    }
  } else {
    // Hors ligne : LED jaune clignotante
    digitalWrite(PIN_LED_YELLOW, (currentTime / 500) % 2);
  }

  delay(10);
}
