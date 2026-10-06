/*
 * ============================================================================
 *  Sentinel-X — firmware du boîtier (ESP32)
 * ============================================================================
 *  Carte      : "ESP32 Dev Module" — core Arduino ESP32 (Espressif) version 3.1 ou plus
 *  Bibliothèques (Arduino IDE > Outils > Gérer les bibliothèques) :
 *    - PubSubClient           (Nick O'Leary)
 *    - DHT sensor library     (Adafruit)  + Adafruit Unified Sensor
 *    - ArduinoJson            (Benoit Blanchon) version 7
 *    - (si USE_OLED = 1) Adafruit SSD1306 + Adafruit GFX Library
 *
 *  Rôle :
 *    - lit DHT22 (température, humidité), MQ-2 (gaz), PIR (mouvement)
 *    - envoie les mesures toutes les 2 s en MQTT chiffré (TLS, port 8883)
 *    - envoie une alerte immédiate quand le PIR détecte un mouvement
 *    - reçoit les commandes du dashboard : buzzer et LEDs
 *    - annonce "online"/"offline" sur le topic status (LWT)
 *
 *  Les mots de passe et le certificat sont dans secrets.h (jamais commité).
 * ============================================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include "secrets.h"

// ============================ CONFIGURATION ================================
#define GROUP_ID "g02"               // numéro de groupe (doit correspondre à l'ACL)
#define USE_OLED 0                   // 1 = écran OLED SSD1306 128x64 branché en I2C

// --- Câblage (voir firmware/README.md) ---
const int PIN_DHT     = 4;    // DHT22 : broche DATA
const int PIN_MQ2_AO  = 34;   // MQ-2 : sortie AO, via pont diviseur (2 résistances identiques)
const int PIN_PIR     = 27;   // PIR HC-SR501 : broche OUT
const int PIN_BUZZER  = 26;   // buzzer ACTIF : broche S (ou I/O)
const int PIN_LED_RED = 25;   // LED rouge (+ résistance ~220 ohms)
const int PIN_LED_YEL = 33;   // LED jaune (+ résistance ~220 ohms)
const int PIN_LED_GRN = -1;   // LED verte : -1 = non câblée
const bool BUZZER_ACTIVE_LOW = false;  // mettre true si le buzzer sonne quand la broche est à 0

// --- Rythmes ---
const unsigned long TELEMETRY_MS     = 2000;    // une mesure toutes les 2 s
const unsigned long WIFI_RETRY_MS    = 15000;   // nouvelle tentative Wi-Fi toutes les 15 s
const unsigned long RECONNECT_MS     = 5000;    // nouvelle tentative MQTT toutes les 5 s
const unsigned long PIR_ALERT_GAP_MS = 10000;   // au plus une alerte mouvement / 10 s
const unsigned long BUZZER_MAX_MS    = 120000;  // le buzzer se coupe seul après 2 min
const unsigned long MQ2_WARMUP_MS    = 60000;   // le MQ-2 chauffe ~1 min avant d'être fiable

// --- Topics MQTT ---
const char* T_TELEMETRY = "sentinelx/" GROUP_ID "/telemetry";
const char* T_ALERTS    = "sentinelx/" GROUP_ID "/alerts";
const char* T_STATUS    = "sentinelx/" GROUP_ID "/status";
const char* T_CMD       = "sentinelx/" GROUP_ID "/cmd";
// ===========================================================================

#if USE_OLED
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oledOk = false;
#endif

WiFiClientSecure net;
PubSubClient mqtt(net);
DHT dht(PIN_DHT, DHT22);

char clientId[40];
unsigned long lastTelemetry = 0, lastWifiTry = 0, lastMqttTry = 0, lastPirAlert = 0;
unsigned long buzzerSince = 0, bootMs = 0;
bool buzzerOn = false;
bool pirPrev = false, pirLatched = false;
String ledMode = "off";               // dernière commande LED reçue
float lastT = NAN, lastH = NAN;
int lastGaz = -1;

// ============================== SORTIES ====================================
void writePin(int pin, bool on) {
  if (pin >= 0) digitalWrite(pin, on ? HIGH : LOW);
}

void setBuzzer(bool on) {
  buzzerOn = on;
  buzzerSince = millis();
  digitalWrite(PIN_BUZZER, (on != BUZZER_ACTIVE_LOW) ? HIGH : LOW);
}

void applyLeds(const String& mode) {
  writePin(PIN_LED_RED, mode == "rouge");
  writePin(PIN_LED_YEL, mode == "jaune");
  writePin(PIN_LED_GRN, mode == "vert");
}

// =============================== CAPTEURS ==================================
int readGaz() {
  long sum = 0;
  for (int i = 0; i < 8; i++) { sum += analogRead(PIN_MQ2_AO); delay(2); }
  return (int)(sum / 8);               // 0..4095 (valeur brute ADC)
}

// ============================ COMMANDES MQTT ===============================
void onMessage(char* topic, byte* payload, unsigned int len) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, len);
  if (err) {
    Serial.printf("[CMD] JSON invalide : %s\n", err.c_str());
    return;
  }

  JsonVariant buz = doc["buzzer"];
  if (!buz.isNull()) {
    bool on = buz.as<int>() == 1;
    setBuzzer(on);
    Serial.printf("[CMD] buzzer %s\n", on ? "ON" : "OFF");
  }

  JsonVariant led = doc["led"];
  if (!led.isNull()) {
    String m = led.as<String>();
    if (m == "rouge" || m == "jaune" || m == "vert" || m == "off") {
      ledMode = m;
      applyLeds(ledMode);
      Serial.printf("[CMD] LED %s\n", m.c_str());
    }
  }
}

// =============================== RÉSEAU ====================================
void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiTry < WIFI_RETRY_MS && lastWifiTry != 0) return;
  lastWifiTry = millis();
  Serial.printf("[WiFi] connexion à %s...\n", WIFI_SSID);
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void ensureMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (millis() - lastMqttTry < RECONNECT_MS && lastMqttTry != 0) return;
  lastMqttTry = millis();

  Serial.printf("[MQTT] connexion TLS à %s:%d...\n", MQTT_HOST, MQTT_PORT);
  // LWT : si le boîtier disparaît, le broker publie "offline" à sa place
  if (mqtt.connect(clientId, MQTT_USER, MQTT_PASSWORD, T_STATUS, 1, true, "offline")) {
    Serial.println("[MQTT] connecté");
    mqtt.publish(T_STATUS, "online", true);
    mqtt.subscribe(T_CMD, 1);
    applyLeds(ledMode);
  } else {
    char tlsErr[120] = {0};
    net.lastError(tlsErr, sizeof(tlsErr));
    Serial.printf("[MQTT] échec, état=%d  TLS=%s\n", mqtt.state(), tlsErr);
    // état -2 : TLS/réseau (IP, port, certificat)   état 4/5 : identifiant ou ACL refusés
  }
}

// ============================== PUBLICATIONS ===============================
void publishTelemetry() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  int gaz = readGaz();
  if (!isnan(t)) lastT = t;
  if (!isnan(h)) lastH = h;
  lastGaz = gaz;

  JsonDocument doc;
  if (isnan(t)) doc["t"] = nullptr; else doc["t"] = roundf(t * 10) / 10.0;
  if (isnan(h)) doc["h"] = nullptr; else doc["h"] = roundf(h);
  doc["gaz"] = gaz;
  doc["pir"] = pirLatched ? 1 : 0;      // mouvement vu depuis la dernière mesure
  doc["chauffe_mq2"] = (millis() - bootMs < MQ2_WARMUP_MS) ? 1 : 0;
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000;

  char buf[256];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  bool ok = mqtt.publish(T_TELEMETRY, (const uint8_t*)buf, n, false);
  Serial.printf("[TX] %s %s\n", buf, ok ? "" : "(ÉCHEC)");
  pirLatched = false;
}

void publishMotionAlert() {
  JsonDocument doc;
  doc["type"] = "mouvement";
  doc["niveau"] = "attention";
  doc["source"] = "pir";
  doc["msg"] = "Mouvement détecté par le PIR";
  char buf[160];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(T_ALERTS, (const uint8_t*)buf, n, false);
  Serial.println("[ALERTE] mouvement");
}

// ================================ OLED =====================================
#if USE_OLED
void drawOled() {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.println("SENTINEL-X " GROUP_ID);
  oled.print("IP  ");
  oled.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("--"));
  oled.print("MQTT ");
  oled.println(mqtt.connected() ? "OK (TLS)" : "deconnecte");
  oled.printf("T %.1fC  H %.0f%%\n", isnan(lastT) ? 0.0 : lastT, isnan(lastH) ? 0.0 : lastH);
  oled.printf("Gaz %d\n", lastGaz);
  oled.printf("Buzzer %s  LED %s\n", buzzerOn ? "ON" : "off", ledMode.c_str());
  oled.display();
}
#endif

// ============================ SETUP / LOOP =================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Sentinel-X ESP32 ===");
  bootMs = millis();

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  if (PIN_LED_RED >= 0) pinMode(PIN_LED_RED, OUTPUT);
  if (PIN_LED_YEL >= 0) pinMode(PIN_LED_YEL, OUTPUT);
  if (PIN_LED_GRN >= 0) pinMode(PIN_LED_GRN, OUTPUT);
  setBuzzer(false);
  applyLeds("off");

  analogReadResolution(12);        // 0..4095 (atténuation par défaut : plage 0..~3,1 V)
  dht.begin();

#if USE_OLED
  Wire.begin(21, 22);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledOk) Serial.println("[OLED] écran non détecté (adresse 0x3C ?)");
#endif

  snprintf(clientId, sizeof(clientId), "esp32-%s-%04X", GROUP_ID,
           (unsigned)(ESP.getEfuseMac() & 0xFFFF));

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  net.setCACert(CA_CERT);          // vérifie que le serveur est bien le nôtre
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(10);
}

void loop() {
  ensureWifi();
  ensureMqtt();
  mqtt.loop();

  // PIR : détection de front montant
  bool pir = digitalRead(PIN_PIR) == HIGH;
  if (pir) pirLatched = true;
  if (pir && !pirPrev && mqtt.connected() && millis() - lastPirAlert > PIR_ALERT_GAP_MS) {
    lastPirAlert = millis();
    publishMotionAlert();
  }
  pirPrev = pir;

  // Mesures périodiques
  if (millis() - lastTelemetry >= TELEMETRY_MS) {
    lastTelemetry = millis();
    if (mqtt.connected()) publishTelemetry();
#if USE_OLED
    drawOled();
#endif
  }

  // Sécurité : coupure auto du buzzer
  if (buzzerOn && millis() - buzzerSince > BUZZER_MAX_MS) setBuzzer(false);

  // Hors ligne : LED jaune clignotante
  if (!mqtt.connected()) {
    writePin(PIN_LED_RED, false);
    writePin(PIN_LED_YEL, (millis() / 500) % 2);
  }

  delay(10);
}
