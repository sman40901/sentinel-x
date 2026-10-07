// =========================================================
// Sentinel-X — NodeMCU LoLin V3 / ESP8266MOD (ESP-12F)
//
// Sensors:   DHT22 (D2), HC-SR501 PIR (D1), MQ gas (A0 via divider),
//            MPU-6500 tamper/tilt (D6/D7)
// Outputs:   green / yellow / red status LEDs, optional buzzer
// Network:   hosts its own AP with a dashboard, optionally joins the
//            server's network to publish MQTT telemetry
// Presence:  WiFi device detection — associated clients + probe sniffer
//
// EVERY tunable lives in include/config.h. Nothing here should need editing
// to change pins, thresholds or timings.
// =========================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

#include "config.h"

#if ENABLE_CLIMATE
  #include <DHT.h>
#endif
#if ENABLE_IMU
  #include "imu.h"
#endif
#if ENABLE_PRESENCE
  #include "presence.h"
#endif
#if ENABLE_WEB_DASHBOARD
  #include "webui.h"
#endif
#if ENABLE_MQTT
  #include <PubSubClient.h>
  #if MQTT_USE_TLS
    #include <WiFiClientSecure.h>
  #endif
#endif

// =========================================================
// Pin sanity checks.
//
// These are static_assert and not #error on purpose: D0..D8 are
// `static const uint8_t` in the ESP8266 core rather than macros, so the
// preprocessor cannot compare them — it sees them as undefined and every
// comparison collapses to 0 == 0.
// =========================================================

#if ENABLE_LEDS
static_assert(PIN_LED_GREEN != PIN_LED_YELLOW
           && PIN_LED_GREEN != PIN_LED_RED
           && PIN_LED_YELLOW != PIN_LED_RED,
              "The three status LEDs must be on three different pins.");
static_assert(PIN_LED_RED == D8,
              "The red LED must be on D8/GPIO15, wired active-high, or the chip "
              "will not boot. If you move it, re-read section 2 of config.h.");
  #if ENABLE_CLIMATE
static_assert(PIN_LED_GREEN != PIN_DHT && PIN_LED_YELLOW != PIN_DHT
           && PIN_LED_RED != PIN_DHT, "An LED pin collides with the DHT data pin.");
  #endif
  #if ENABLE_MOTION
static_assert(PIN_LED_GREEN != PIN_PIR && PIN_LED_YELLOW != PIN_PIR
           && PIN_LED_RED != PIN_PIR, "An LED pin collides with the PIR pin.");
  #endif
  #if ENABLE_IMU
static_assert(PIN_LED_GREEN != PIN_IMU_SDA && PIN_LED_GREEN != PIN_IMU_SCL
           && PIN_LED_YELLOW != PIN_IMU_SDA && PIN_LED_YELLOW != PIN_IMU_SCL
           && PIN_LED_RED != PIN_IMU_SDA && PIN_LED_RED != PIN_IMU_SCL,
              "An LED pin collides with the I2C bus (D6/D7).");
  #endif
#endif

#if ENABLE_IMU
static_assert(PIN_IMU_SDA != PIN_IMU_SCL, "SDA and SCL cannot share a pin.");
#endif

#if ENABLE_BUZZER
static_assert(PIN_BUZZER == D4,
              "The buzzer must be on D4/GPIO2, active-low, switched by a PNP high-side "
              "transistor. GPIO2 must be HIGH at reset; an NPN low-side switch would "
              "clamp it low and the board would not boot. See config.h section 2.");
static_assert(PIN_BUZZER != PIN_LED_GREEN && PIN_BUZZER != PIN_LED_YELLOW
           && PIN_BUZZER != PIN_LED_RED, "The buzzer collides with an LED pin.");
#endif

// =========================================================
// Threat levels — these map 1:1 onto the three LEDs
// =========================================================

enum ThreatLevel : uint8_t { THREAT_GREEN = 0, THREAT_YELLOW = 1, THREAT_RED = 2 };

// Forward declarations. This is a .cpp, so unlike an .ino the build does not
// generate prototypes for us and definition order would otherwise matter.
static bool sensorsHealthy();
static float gasDelta();
static bool pirWarming();
static bool gasWarming();
static void fmtFloat(char *out, size_t n, float v, int dp);
#if ENABLE_PRESENCE && PRESENCE_SNIFFER
static void rebuildNetwork();
#endif

static ThreatLevel threatLevel = THREAT_GREEN;
static char threatReason[112] = "";
static unsigned long redUntil = 0;
static unsigned long yellowUntil = 0;

// =========================================================
// Sensor state
//
// The loop owns all sampling; the web and MQTT paths only ever report the
// last values measured. That keeps the DHT's 2 s minimum interval honest no
// matter how fast a browser polls.
// =========================================================

#if ENABLE_CLIMATE
DHT dht(PIN_DHT, DHT_TYPE);
#endif
static float lastCelsius = NAN;
static float lastHumidity = NAN;
static bool climateHealthy = false;

static int   lastGasRaw = 0;
static float lastGasVolts = NAN;
static float gasBaseline = NAN;
static float baselineSum = 0;
static int   baselineSamples = 0;

static int lastMotion = LOW;

#if ENABLE_IMU
static ImuReading imu = {};
static uint8_t imuWho = 0;
static bool imuOk = false;
static bool imuTamper = false;
// Orientation the enclosure was in at startup. Tamper is measured against
// this, not against level - see TILT_WARN_DEG in config.h.
static float imuRefPitch = NAN;
static float imuRefRoll = NAN;
#endif

#if ENABLE_PRESENCE
static PresenceStats presence = {};
#endif

static unsigned long lastClimateRead = 0;
static unsigned long lastGasRead = 0;
#if ENABLE_IMU
static unsigned long lastImuRead = 0;
#endif
static unsigned long lastLedRefresh = 0;
static unsigned long lastSerialLog = 0;

// Manual LED override, driven by an MQTT command. Expires on its own so a
// forgotten test command cannot leave the box lying about its state.
static unsigned long ledOverrideUntil = 0;
static bool ledOverride[3] = {false, false, false};   // green, yellow, red

static unsigned long buzzerOverrideUntil = 0;
static bool buzzerOverrideState = false;

#if ENABLE_WEB_DASHBOARD
ESP8266WebServer server(WEB_PORT);
#endif

// =========================================================
// MQTT
// =========================================================

#if ENABLE_MQTT
  #if MQTT_USE_TLS
BearSSL::WiFiClientSecure netClient;
// Parsed once at boot. The board has no RTC and an isolated AP has no NTP, so
// setX509Time() below is what makes notBefore/notAfter checks pass at all.
BearSSL::X509List caCertList(CA_CERT);
  #else
WiFiClient netClient;
  #endif

PubSubClient mqtt(netClient);
static unsigned long lastMqttTry = 0;
static unsigned long lastTelemetry = 0;
static unsigned long lastGasAlert = 0;
static unsigned long lastMotionAlert = 0;
  #if ENABLE_IMU
static unsigned long lastTamperAlert = 0;
  #endif
  #if ENABLE_PRESENCE
static unsigned long lastPresenceAlert = 0;
  #endif
#endif

// =========================================================
// LEDs
// =========================================================

static void ledWrite(uint8_t pin, bool on) {
#if ENABLE_LEDS
  digitalWrite(pin, on ? HIGH : LOW);   // all three LEDs are active-high
#else
  (void)pin; (void)on;
#endif
}

// Raw level, honouring BUZZER_ACTIVE_LOW so the wiring can change without
// touching anything else. As built this board is ACTIVE-HIGH:
// D4 -> 100R -> buzzer -> GND, so HIGH sounds it.
static void buzzerWrite(bool on) {
#if ENABLE_BUZZER
  #if BUZZER_ACTIVE_LOW
  digitalWrite(PIN_BUZZER, on ? LOW : HIGH);
  #else
  digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
  #endif
#else
  (void)on;
#endif
}

// Pattern engine, driven off millis() so nothing here ever blocks.
//
// An active buzzer has one pitch and one volume, so rhythm is the only
// channel available to distinguish the states by ear:
//
//   RED     a burst of 3 beeps, then a pause - urgent, hard to ignore
//   YELLOW  one 100 ms chirp every 4 s        - present, easy to live with
//   GREEN   silent. A box that beeps when nothing is wrong gets taped over.
//
// RED is a burst and not a plain duty cycle on purpose: a fast on/off does not
// read as a rhythm. Chopping an active buzzer at ~3 Hz just sounds like one
// continuous warble, which is exactly how the first version came out on the
// bench. Beeps need to be ~120 ms with a real pause between bursts to register
// as separate events by ear.
static void applyBuzzer() {
#if ENABLE_BUZZER
  const unsigned long now = millis();

  if (now < buzzerOverrideUntil) {
    buzzerWrite(buzzerOverrideState);
    return;
  }

  switch (threatLevel) {
    case THREAT_RED: {
      // One beat = beep + gap. The burst is BEEPS beats, then a pause.
      const unsigned long beat  = BUZZER_RED_BEEP_MS + BUZZER_RED_GAP_MS;
      const unsigned long burst = beat * BUZZER_RED_BEEPS;
      const unsigned long cycle = burst + BUZZER_RED_PAUSE_MS;
      const unsigned long t = now % cycle;
      buzzerWrite(t < burst && (t % beat) < BUZZER_RED_BEEP_MS);
      break;
    }
    case THREAT_YELLOW:
      buzzerWrite((now % BUZZER_YELLOW_PERIOD_MS) < BUZZER_YELLOW_ON_MS);
      break;
    case THREAT_GREEN:
    default:
      buzzerWrite(false);
      break;
  }
#endif
}

// Called on a timer so the blink patterns are smooth.
static void applyLeds() {
#if ENABLE_LEDS
  const unsigned long now = millis();

  if (now < ledOverrideUntil) {
    ledWrite(PIN_LED_GREEN,  ledOverride[0]);
    ledWrite(PIN_LED_YELLOW, ledOverride[1]);
    ledWrite(PIN_LED_RED,    ledOverride[2]);
    return;
  }

  const bool halfSecond = (now / 500) % 2;
  const bool fastBlink  = (now / 150) % 2;

  switch (threatLevel) {
    case THREAT_RED:
      // Fast blink: unmistakable across a room, and distinct from yellow.
      ledWrite(PIN_LED_GREEN, false);
      ledWrite(PIN_LED_YELLOW, false);
      ledWrite(PIN_LED_RED, fastBlink);
      break;

    case THREAT_YELLOW:
      ledWrite(PIN_LED_GREEN, false);
      ledWrite(PIN_LED_YELLOW, halfSecond);
      ledWrite(PIN_LED_RED, false);
      break;

    case THREAT_GREEN:
    default:
      // Solid green means validated. Blinking green means we are running but a
      // sensor is not answering — visible without opening the dashboard.
      ledWrite(PIN_LED_GREEN, sensorsHealthy() ? true : halfSecond);
      ledWrite(PIN_LED_YELLOW, false);
      ledWrite(PIN_LED_RED, false);
      break;
  }
#endif
}

// =========================================================
// Health
// =========================================================

static bool pirWarming() {
  return millis() < PIR_WARMUP_MS;
}

static bool gasWarming() {
  return isnan(gasBaseline);
}

static bool sensorsHealthy() {
#if ENABLE_CLIMATE
  if (!climateHealthy) return false;
#endif
#if ENABLE_IMU
  if (!imuOk) return false;
#endif
  return true;
}

static float gasDelta() {
  if (isnan(gasBaseline) || isnan(lastGasVolts)) return NAN;
  return lastGasVolts - gasBaseline;
}

// =========================================================
// Threat state machine
//
// Yellow and red latch for a hold time so a one-frame trip is still visible
// to someone glancing at the box, instead of flickering past.
// =========================================================

static void evaluateThreat() {
  const unsigned long now = millis();
  char reason[112] = "";

  bool red = false;
  bool yellow = false;

  // --- confirmed intrusion -> red ---
#if ENABLE_MOTION
  if (lastMotion == HIGH && !pirWarming()) {
    red = true;
    strncpy(reason, "PIR motion confirmed", sizeof(reason) - 1);
  }
#endif

#if ENABLE_GAS
  const float delta = gasDelta();
  if (!isnan(delta) && delta >= GAS_CRIT_DELTA_V) {
    red = true;
    snprintf(reason, sizeof(reason), "Gas %+.2f V above baseline (critical)", delta);
  }
#endif

#if ENABLE_PRESENCE
  if (presence.score >= PRESENCE_CRIT_SCORE) {
    red = true;
    snprintf(reason, sizeof(reason), "WiFi presence score %u (crowded)", presence.score);
  }
#endif

  // --- unconfirmed, something is out there -> yellow ---
#if ENABLE_PRESENCE
  if (!red && presence.score >= PRESENCE_WARN_SCORE) {
    yellow = true;
    snprintf(reason, sizeof(reason), "%u device(s) detected nearby by WiFi",
             presence.associated + presence.sniffedTotal);
  }
#endif

#if ENABLE_GAS
  if (!red && !isnan(delta) && delta >= GAS_WARN_DELTA_V) {
    yellow = true;
    snprintf(reason, sizeof(reason), "Gas %+.2f V above baseline", delta);
  }
#endif

#if ENABLE_IMU
  if (imuOk && imuTamper) {
    if (!red) {
      yellow = true;
      snprintf(reason, sizeof(reason), "Enclosure moved %.0f deg from its startup position",
               max(imuAngleDelta(imu.pitch, imuRefPitch),
                   imuAngleDelta(imu.roll, imuRefRoll)));
    }
  }
#endif

  if (red)    redUntil    = now + INTRUDER_HOLD_MS;
  if (yellow) yellowUntil = now + WARNING_HOLD_MS;
  if (red || yellow) {
    strncpy(threatReason, reason, sizeof(threatReason) - 1);
    threatReason[sizeof(threatReason) - 1] = '\0';
  }

  if (now < redUntil) {
    threatLevel = THREAT_RED;
  } else if (now < yellowUntil) {
    threatLevel = THREAT_YELLOW;
  } else {
    threatLevel = THREAT_GREEN;
    if (!sensorsHealthy()) {
      strncpy(threatReason, "Running, but a sensor is not answering", sizeof(threatReason) - 1);
    } else {
      threatReason[0] = '\0';
    }
  }
}

// =========================================================
// Sensors
// =========================================================

#if ENABLE_MOTION
static void pollMotion() {
  const int motion = digitalRead(PIN_PIR);
  if (motion == lastMotion) return;
  lastMotion = motion;

  if (motion == HIGH) {
    Serial.println(pirWarming() ? F("[pir] motion (still warming up, ignored)")
                                : F("[pir] MOTION DETECTED"));
  } else {
    Serial.println(F("[pir] clear"));
  }
}
#endif

#if ENABLE_GAS
static void readGas() {
  // Average a handful of samples: the MQ's output is noisy and a single
  // reading jitters enough to cross a threshold on its own.
  long sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(PIN_GAS);
    delay(2);
  }
  lastGasRaw = (int)(sum / 8);

  // Undo the divider so the figure quoted is what the sensor's AO pin is
  // actually doing, not what survived the two resistors.
  const float atPin = lastGasRaw * ADC_FULL_SCALE_V / ADC_MAX;
  lastGasVolts = atPin * GAS_DIVIDER_RATIO;

  if (!isnan(gasBaseline) || millis() < GAS_WARMUP_MS) return;

  baselineSum += lastGasVolts;
  if (++baselineSamples < GAS_BASELINE_SAMPLES) return;

  gasBaseline = baselineSum / baselineSamples;
  Serial.printf("[gas] baseline settled at %.2f V\n", gasBaseline);
}
#endif

#if ENABLE_CLIMATE
static void readClimate() {
  const float humidity = dht.readHumidity();
  const float celsius = dht.readTemperature();

  if (isnan(humidity) || isnan(celsius)) {
    climateHealthy = false;
    Serial.println(F("[dht] read failed - check DATA on D2 and VCC on the 3.3 V island"));
    return;
  }

  lastHumidity = humidity;
  lastCelsius = celsius;
  climateHealthy = true;
}
#endif

#if ENABLE_IMU
static void readImu() {
  if (!imuOk) return;

  // A failed read is reported but never latched off: a knocked wire that gets
  // pushed back in should recover on the next pass.
  if (!imuRead(&imu)) {
    Serial.println(F("[imu] read failed - check SDA on D6, SCL on D7"));
    return;
  }

  // Capture the mounting orientation once, after it has had time to settle.
  if (isnan(imuRefPitch) && millis() > IMU_BASELINE_MS) {
    imuRefPitch = imu.pitch;
    imuRefRoll  = imu.roll;
    Serial.printf("[imu] reference orientation captured: pitch %.1f, roll %.1f\n",
                  imuRefPitch, imuRefRoll);
  }

  imuTamper = imuTiltedFrom(imu, imuRefPitch, imuRefRoll) || imuShocked(imu);
}
#endif

// =========================================================
// Network
// =========================================================

static void startAccessPoint() {
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[ap]   hosting \"%s\"  ->  http://%s/\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());
}

#if WIFI_JOIN_STATION
static void joinStation(bool verbose) {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  if (verbose) Serial.printf("[sta]  joining %s", WIFI_SSID);

  const unsigned long deadline = millis() + WIFI_STA_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(500);
    if (verbose) Serial.print('.');
  }
  if (verbose) Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    if (verbose) Serial.printf("[sta]  connected, ip %s, rssi %d dBm\n",
                               WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else if (verbose) {
    // Not fatal: the sensors, LEDs and local dashboard all work regardless.
    Serial.println(F("[sta]  failed. 2.4 GHz only, and WPA2-PSK only -"));
    Serial.println(F("       WiFi.begin() cannot join WPA2-Enterprise networks."));
  }
}
#endif

#if ENABLE_PRESENCE && PRESENCE_SNIFFER
// Rebuilt from scratch after each sniff window, which tears the radios down.
static void rebuildNetwork() {
#if WIFI_JOIN_STATION
  WiFi.mode(WIFI_AP_STA);
#else
  WiFi.mode(WIFI_AP);
#endif
  delay(10);
  WiFi.softAP(AP_SSID, AP_PASS);
#if WIFI_JOIN_STATION
  joinStation(false);   // quiet: this happens every SNIFF_PERIOD_MS
#endif
}
#endif  // ENABLE_PRESENCE && PRESENCE_SNIFFER

// =========================================================
// Web dashboard served by the board itself
// =========================================================

// Fixed buffers rather than String, so repeated polling cannot fragment the
// heap. NaN becomes JSON null. Shared by the web and MQTT paths, so it lives
// outside both feature guards.
static void fmtFloat(char *out, size_t n, float v, int dp) {
  if (isnan(v)) {
    strncpy(out, "null", n);
    out[n - 1] = '\0';
    return;
  }
  dtostrf(v, 0, dp, out);
}

#if ENABLE_WEB_DASHBOARD
static void handleRoot() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

static void handleReadings() {
  char t[12], h[12], gv[12], gd[12], pitch[12], roll[12], mag[12];

  fmtFloat(t, sizeof(t), lastCelsius, 1);
  fmtFloat(h, sizeof(h), lastHumidity, 1);
  fmtFloat(gv, sizeof(gv), lastGasVolts, 2);
  fmtFloat(gd, sizeof(gd), gasDelta(), 2);

#if ENABLE_IMU
  fmtFloat(pitch, sizeof(pitch), imuOk ? imu.pitch : NAN, 1);
  fmtFloat(roll, sizeof(roll), imuOk ? imu.roll : NAN, 1);
  fmtFloat(mag, sizeof(mag), imuOk ? imu.magnitude : NAN, 2);
#else
  strcpy(pitch, "null"); strcpy(roll, "null"); strcpy(mag, "null");
#endif

  const float delta = gasDelta();
  const char *threatName = threatLevel == THREAT_RED ? "red"
                         : threatLevel == THREAT_YELLOW ? "yellow" : "green";

  // Mirrors what applyLeds() is driving, so the on-screen bulbs match the
  // physical ones instead of being recomputed in the browser.
  const bool gLed = threatLevel == THREAT_GREEN;
  const bool yLed = threatLevel == THREAT_YELLOW;
  const bool rLed = threatLevel == THREAT_RED;

  char json[900];
  int n = snprintf(json, sizeof(json),
    "{\"threat\":\"%s\",\"reason\":\"%s\","
    "\"led\":{\"green\":%s,\"yellow\":%s,\"red\":%s},"
    "\"tempC\":%s,\"humidity\":%s,"
    "\"motion\":%s,\"pirWarming\":%s,"
    "\"gasRaw\":%d,\"gasVolts\":%s,\"gasDelta\":%s,"
    "\"gasWarming\":%s,\"gasWarn\":%s,\"gasCrit\":%s,"
    "\"imuOk\":%s,\"imuWho\":%u,\"pitch\":%s,\"roll\":%s,\"imuMag\":%s,\"tampered\":%s,",
    threatName, threatReason,
    gLed ? "true" : "false", yLed ? "true" : "false", rLed ? "true" : "false",
    t, h,
    lastMotion == HIGH ? "true" : "false", pirWarming() ? "true" : "false",
    lastGasRaw, gv, gd,
    gasWarming() ? "true" : "false",
    (!isnan(delta) && delta >= GAS_WARN_DELTA_V) ? "true" : "false",
    (!isnan(delta) && delta >= GAS_CRIT_DELTA_V) ? "true" : "false",
#if ENABLE_IMU
    imuOk ? "true" : "false", imuWho, pitch, roll, mag, imuTamper ? "true" : "false"
#else
    "false", 0, pitch, roll, mag, "false"
#endif
  );

#if ENABLE_PRESENCE
  const unsigned long sinceSniff = millis() - presence.lastSniffMs;
  const unsigned long nextSniff = sinceSniff >= SNIFF_PERIOD_MS ? 0
                                : (SNIFF_PERIOD_MS - sinceSniff) / 1000;
  n += snprintf(json + n, sizeof(json) - n,
    "\"presence\":{\"score\":%u,\"associated\":%u,\"sniffed\":%u,"
    "\"randomized\":%u,\"stable\":%u,\"strongestRssi\":%d,\"frames\":%lu,"
    "\"sniffer\":%s,\"nextSniffS\":%lu,\"windowMs\":%lu,"
    "\"warnScore\":%u,\"critScore\":%u},",
    presence.score, presence.associated, presence.sniffedTotal,
    presence.sniffedRandom, presence.sniffedStable, presence.strongestRssi,
    (unsigned long)presence.framesSeen,
    PRESENCE_SNIFFER ? "true" : "false", nextSniff, SNIFF_WINDOW_MS,
    PRESENCE_WARN_SCORE, PRESENCE_CRIT_SCORE);
#else
  n += snprintf(json + n, sizeof(json) - n,
    "\"presence\":{\"score\":0,\"associated\":0,\"sniffed\":0,\"randomized\":0,"
    "\"stable\":0,\"strongestRssi\":0,\"frames\":0,\"sniffer\":false,"
    "\"nextSniffS\":0,\"windowMs\":0,\"warnScore\":1,\"critScore\":1},");
#endif

#if ENABLE_MQTT
  n += snprintf(json + n, sizeof(json) - n,
    "\"mqttEnabled\":true,\"mqttUp\":%s,\"mqttState\":%d,\"mqttTls\":%s,"
    "\"mqttHost\":\"%s\",\"mqttPort\":%d,",
    mqtt.connected() ? "true" : "false", mqtt.state(),
    MQTT_USE_TLS ? "true" : "false", MQTT_HOST, MQTT_PORT);
#else
  n += snprintf(json + n, sizeof(json) - n, "\"mqttEnabled\":false,");
#endif

  snprintf(json + n, sizeof(json) - n,
    "\"mode\":\"%s\",\"apIp\":\"%s\",\"heap\":%u,\"uptime\":%lu}",
    WIFI_JOIN_STATION ? "apsta" : "ap",
    WiFi.softAPIP().toString().c_str(),
    (unsigned)ESP.getFreeHeap(), millis() / 1000);

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}
#endif

// =========================================================
// MQTT
// =========================================================

#if ENABLE_MQTT

// Minimal JSON field lookup. ArduinoJson would cost heap we do not have spare
// once BearSSL has taken its share.
static const char *jsonField(const char *json, const char *key) {
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(json, pattern);
  if (!p) return nullptr;
  p = strchr(p + strlen(pattern), ':');
  if (!p) return nullptr;
  p++;
  while (*p == ' ') p++;
  return p;
}

static void publishAlert(const char *type, const char *level, const char *msg) {
  if (!mqtt.connected()) return;

  char alert[200];
  snprintf(alert, sizeof(alert),
           "{\"type\":\"%s\",\"niveau\":\"%s\",\"msg\":\"%s\",\"source\":\"esp8266\"}",
           type, level, msg);
  mqtt.publish(TOPIC_ALERTS, alert);
  Serial.printf("[mqtt] alert %s\n", alert);
}

// Commands: {"buzzer":1} and/or {"led":"rouge"}.
// An LED command takes manual control for LED_OVERRIDE_MS and then hands it
// back to the threat state machine on its own.
static void mqttCallback(char *topic, byte *payload, unsigned int length) {
  char message[160];
  const unsigned int n = length < sizeof(message) - 1 ? length : sizeof(message) - 1;
  memcpy(message, payload, n);
  message[n] = '\0';

  Serial.printf("[mqtt] rx [%s] %s\n", topic, message);
  if (strcmp(topic, TOPIC_CMD) != 0) return;

  // Bounded, like the LED override: a forgotten test command must not leave
  // the box sounding indefinitely.
  const char *b = jsonField(message, "buzzer");
  if (b) {
    buzzerOverrideState = atoi(b) == 1;
    buzzerOverrideUntil = millis() + BUZZER_OVERRIDE_MS;
    Serial.printf("[buzz] manual override %s for %lu ms\n",
                  buzzerOverrideState ? "ON" : "OFF", BUZZER_OVERRIDE_MS);
  }

  const char *l = jsonField(message, "led");
  if (l && *l == '"') {
    char color[16] = {0};
    const char *end = strchr(l + 1, '"');
    if (end && end - (l + 1) < (int)sizeof(color)) {
      memcpy(color, l + 1, end - (l + 1));
    }

    ledOverride[0] = ledOverride[1] = ledOverride[2] = false;
    if      (strcmp(color, "vert")  == 0) ledOverride[0] = true;
    else if (strcmp(color, "jaune") == 0) ledOverride[1] = true;
    else if (strcmp(color, "rouge") == 0) ledOverride[2] = true;
    else if (strcmp(color, "blanc") == 0) ledOverride[0] = ledOverride[1] = ledOverride[2] = true;
    // "eteint" / "off" / anything else: all three stay dark.

    ledOverrideUntil = millis() + LED_OVERRIDE_MS;
    Serial.printf("[led]  manual override \"%s\" for %lu ms\n", color, LED_OVERRIDE_MS);
  }

  if (jsonField(message, "auto")) {
    ledOverrideUntil = 0;         // hand control straight back
    buzzerOverrideUntil = 0;
    Serial.println(F("[led]  overrides cleared"));
  }
}

static void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;

  Serial.printf("[mqtt] connecting to %s:%d (%s) ... ",
                MQTT_HOST, MQTT_PORT, MQTT_USE_TLS ? "TLS" : "plain");

  char clientId[40];
  snprintf(clientId, sizeof(clientId), "sentinelx-%s-esp8266", GROUP_ID);

  // LWT: if the box vanishes, the broker publishes "offline" on our behalf.
  if (mqtt.connect(clientId, MQTT_USERNAME, MQTT_PASSWORD,
                   TOPIC_STATUS, 1, true, "offline")) {
    Serial.println(F("connected"));
    mqtt.subscribe(TOPIC_CMD, 1);
    mqtt.publish(TOPIC_STATUS, "online", true);
    return;
  }

  Serial.printf("failed, state=%d  (heap %u)\n", mqtt.state(), ESP.getFreeHeap());
#if MQTT_USE_TLS
  char err[80] = {0};
  netClient.getLastSSLError(err, sizeof(err));
  if (err[0]) Serial.printf("[mqtt] tls: %s\n", err);
  // state=-2 is the catch-all for network/TLS trouble: wrong IP, port 8883
  // blocked by the Windows firewall, a bad ca.crt paste, a certificate whose
  // dates straddle BUILD_EPOCH, or simply not enough heap left for the
  // handshake. state=4/5 means the username, password or ACL is wrong.
#endif
}

static void publishTelemetry() {
  if (!mqtt.connected()) return;

  // The API requires t, h, gaz and pir to be present. t and h are sent as
  // JSON null when the DHT has not produced a good reading, so one dead
  // sensor does not throw away the gas and motion data alongside it.
  char t[12], h[12];
  fmtFloat(t, sizeof(t), lastCelsius, 1);
  fmtFloat(h, sizeof(h), lastHumidity, 0);

  const float delta = gasDelta();
  char gd[12];
  fmtFloat(gd, sizeof(gd), delta, 2);

  const char *threatName = threatLevel == THREAT_RED ? "red"
                         : threatLevel == THREAT_YELLOW ? "yellow" : "green";

  char payload[320];
  snprintf(payload, sizeof(payload),
    // gaz is the raw ADC value. NOTE: 0-1023 on this chip, not 0-4095 as on
    // the ESP32 this project used to target. Consumers must scale accordingly.
    "{\"t\":%s,\"h\":%s,\"gaz\":%d,\"pir\":%d,\"rssi\":%d,"
    "\"gaz_v\":%.2f,\"gaz_d\":%s,"
    "\"pres\":%u,\"assoc\":%u,\"sniff\":%u,"
    "\"tilt\":%d,\"state\":\"%s\",\"heap\":%u}",
    t, h, lastGasRaw, lastMotion == HIGH ? 1 : 0, WiFi.RSSI(),
    isnan(lastGasVolts) ? 0.0f : lastGasVolts, gd,
#if ENABLE_PRESENCE
    presence.score, presence.associated, presence.sniffedTotal,
#else
    0, 0, 0,
#endif
#if ENABLE_IMU
    imuTamper ? 1 : 0,
#else
    0,
#endif
    threatName, (unsigned)ESP.getFreeHeap());

  if (!mqtt.publish(TOPIC_TELEMETRY, payload)) {
    Serial.printf("[mqtt] publish failed (payload %u B, heap %u)\n",
                  strlen(payload), ESP.getFreeHeap());
  }
}

// Alerts are rate-limited per type so a sustained condition does not flood
// the broker.
static void publishAlerts() {
  const unsigned long now = millis();

#if ENABLE_GAS
  const float delta = gasDelta();
  if (!isnan(delta) && delta >= GAS_CRIT_DELTA_V && now - lastGasAlert > ALERT_COOLDOWN_MS) {
    lastGasAlert = now;
    publishAlert("gaz", "critique", "Seuil gaz depasse au-dessus de la baseline");
  }
#endif

#if ENABLE_MOTION
  if (lastMotion == HIGH && !pirWarming() && now - lastMotionAlert > ALERT_COOLDOWN_MS) {
    lastMotionAlert = now;
    publishAlert("mouvement", "attention", "Mouvement detecte par le PIR");
  }
#endif

#if ENABLE_IMU
  if (imuOk && imuTamper && now - lastTamperAlert > ALERT_COOLDOWN_MS) {
    lastTamperAlert = now;
    publishAlert("sabotage", "critique", "Boitier incline ou choque");
  }
#endif

#if ENABLE_PRESENCE
  if (presence.score >= PRESENCE_WARN_SCORE && now - lastPresenceAlert > ALERT_COOLDOWN_MS) {
    lastPresenceAlert = now;
    char msg[96];
    snprintf(msg, sizeof(msg), "Presence WiFi: %u associes, %u sondes",
             presence.associated, presence.sniffedTotal);
    publishAlert("presence", presence.score >= PRESENCE_CRIT_SCORE ? "critique" : "attention", msg);
  }
#endif
}
#endif  // ENABLE_MQTT

// =========================================================
// Serial log
// =========================================================

static void serialLog() {
  Serial.printf("[%6lus] %-6s | %.1fC %.0f%%RH | pir=%d | gas %.2fV (d%+.2f) | ",
                millis() / 1000,
                threatLevel == THREAT_RED ? "RED" : threatLevel == THREAT_YELLOW ? "YELLOW" : "green",
                lastCelsius, lastHumidity, lastMotion,
                isnan(lastGasVolts) ? 0.0f : lastGasVolts,
                isnan(gasDelta()) ? 0.0f : gasDelta());
#if ENABLE_IMU
  Serial.printf("imu p%.0f r%.0f | ", imuOk ? imu.pitch : 0.0f, imuOk ? imu.roll : 0.0f);
#endif
#if ENABLE_PRESENCE
  Serial.printf("presence %u (%u assoc, %u sniffed) | ",
                presence.score, presence.associated, presence.sniffedTotal);
#endif
  Serial.printf("heap %u\n", ESP.getFreeHeap());

  if (threatReason[0]) Serial.printf("          reason: %s\n", threatReason);
}

// =========================================================
// Setup
// =========================================================

void setup() {
#if ENABLE_BUZZER
  // First action in the whole program. GPIO2's strap pull-up holds the pin high
  // from reset, which sounds an active-high buzzer, so the sooner this runs the
  // shorter the boot chirp.
  pinMode(PIN_BUZZER, OUTPUT);
  buzzerWrite(false);
#endif

  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println();
  Serial.println(F("================ Sentinel-X ================"));
  Serial.printf("board   : ESP8266 (ESP-12F), chip id %08X\n", ESP.getChipId());
  Serial.printf("build   : %s %s\n", __DATE__, __TIME__);

#if ENABLE_LEDS
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  // Brief self-test: if a LED never lights here it is wired wrong, not idle.
  ledWrite(PIN_LED_GREEN, true);  delay(200);
  ledWrite(PIN_LED_YELLOW, true); delay(200);
  ledWrite(PIN_LED_RED, true);    delay(200);
  ledWrite(PIN_LED_GREEN, false);
  ledWrite(PIN_LED_YELLOW, false);
  ledWrite(PIN_LED_RED, false);
  Serial.println(F("leds    : green D5, yellow D0, red D8 (all active-high)"));
#endif

#if ENABLE_BUZZER
  // Already initialised at the very top of setup(). One short beep, so a silent
  // buzzer is known to be a wiring fault rather than nothing having triggered.
  buzzerWrite(true);  delay(120);
  buzzerWrite(false);
  Serial.printf("buzzer  : active, D4/GPIO2, active-%s\n",
                BUZZER_ACTIVE_LOW ? "low" : "high");
#endif

#if ENABLE_CLIMATE
  dht.begin();
  Serial.printf("climate : %s on D2\n", DHT_TYPE == DHT22 ? "DHT22" : "DHT11");
#endif
#if ENABLE_MOTION
  pinMode(PIN_PIR, INPUT);
  Serial.println(F("motion  : HC-SR501 on D1, ignoring trips for 60 s"));
#endif
#if ENABLE_GAS
  Serial.println(F("gas     : MQ on A0 via 68k+68k, baseline after 3 min"));
#endif
#if ENABLE_IMU
  imuWho = imuBegin();
  imuOk = imuWho != 0;
  if (imuOk) {
    Serial.printf("imu     : WHO_AM_I=0x%02X on D6/D7%s\n", imuWho,
                  imuWho == 0x70 ? " (MPU-6500, no magnetometer)" : "");
  } else {
    Serial.println(F("imu     : not responding on D6/D7 - tamper detection off"));
  }
#endif

  // --- radios ---
#if WIFI_JOIN_STATION
  WiFi.mode(WIFI_AP_STA);
#else
  WiFi.mode(WIFI_AP);
#endif
  WiFi.setAutoReconnect(true);
  startAccessPoint();
#if WIFI_JOIN_STATION
  joinStation(true);
#endif

#if ENABLE_WEB_DASHBOARD
  server.on("/", handleRoot);
  server.on("/api/readings", handleReadings);
  server.begin();
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", WEB_PORT);
    Serial.printf("web     : http://%s.local/ and http://%s/\n",
                  MDNS_HOSTNAME, WiFi.softAPIP().toString().c_str());
  }
#endif

#if ENABLE_PRESENCE
  presenceReset();
  Serial.printf("presence: assoc + %s (window %lu ms every %lu ms)\n",
                PRESENCE_SNIFFER ? "probe sniffer" : "sniffer DISABLED",
                SNIFF_WINDOW_MS, SNIFF_PERIOD_MS);
  #if PRESENCE_SNIFFER
  Serial.println(F("          NOTE: each sniff window drops the AP and the MQTT link."));
  #endif
#endif

#if ENABLE_MQTT
  #if MQTT_USE_TLS
  netClient.setTrustAnchors(&caCertList);
  // No RTC and no internet on an isolated AP, so NTP cannot help. Seed the
  // clock from the build date; bump BUILD_EPOCH in config.h if the server's
  // certificate starts reading as not-yet-valid.
  netClient.setX509Time(BUILD_EPOCH);

  // BearSSL defaults to a 16 kB receive buffer, which this chip cannot really
  // afford. Shrink it only if the broker actually negotiates MFLN - forcing
  // small buffers against a server that does not support it gives you
  // handshakes that fail on large certificate chains instead.
  if (netClient.probeMaxFragmentLength(MQTT_HOST, MQTT_PORT, MQTT_TLS_RX_BUFFER)) {
    netClient.setBufferSizes(MQTT_TLS_RX_BUFFER, MQTT_TLS_TX_BUFFER);
    Serial.printf("mqtt    : MFLN ok, TLS buffers %d/%d B\n",
                  MQTT_TLS_RX_BUFFER, MQTT_TLS_TX_BUFFER);
  } else {
    Serial.println(F("mqtt    : no MFLN from broker - TLS keeps its 16 kB buffer."));
    Serial.println(F("          Watch the heap; set MQTT_USE_TLS 0 if it resets."));
  }
  #endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
  mqtt.setSocketTimeout(10);
  connectMqtt();
#endif

  Serial.printf("heap    : %u B free\n", ESP.getFreeHeap());
  Serial.println(F("============================================"));
}

// =========================================================
// Loop
// =========================================================

void loop() {
  const unsigned long now = millis();

#if ENABLE_WEB_DASHBOARD
  server.handleClient();
  MDNS.update();
#endif

#if ENABLE_MOTION
  pollMotion();     // every pass, so a brief trip is never missed
#endif

#if ENABLE_CLIMATE
  if (now - lastClimateRead >= CLIMATE_INTERVAL_MS) {
    lastClimateRead = now;
    readClimate();
  }
#endif

#if ENABLE_GAS
  if (now - lastGasRead >= GAS_INTERVAL_MS) {
    lastGasRead = now;
    readGas();
  }
#endif

#if ENABLE_IMU
  if (now - lastImuRead >= IMU_INTERVAL_MS) {
    lastImuRead = now;
    readImu();
  }
#endif

#if ENABLE_PRESENCE
  presence = presenceCollect();

  #if PRESENCE_SNIFFER
  // Scheduled last in the sensor block: this blocks for SNIFF_WINDOW_MS and
  // takes the AP down with it, so everything else gets serviced first.
  if (presenceSniffDue()) {
    Serial.println(F("[sniff] window open - AP down briefly"));
    presenceRunSniffWindow(rebuildNetwork);
    presence = presenceCollect();
    Serial.printf("[sniff] %u MACs (%u random, %u stable), %lu frames\n",
                  presence.sniffedTotal, presence.sniffedRandom,
                  presence.sniffedStable, (unsigned long)presence.framesSeen);
  }
  #endif
#endif

  evaluateThreat();

  if (now - lastLedRefresh >= LED_REFRESH_MS) {
    lastLedRefresh = now;
    applyLeds();
    applyBuzzer();
  }

#if ENABLE_MQTT
  if (mqtt.connected()) {
    mqtt.loop();
    if (now - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
      lastTelemetry = now;
      publishTelemetry();
      publishAlerts();
    }
  } else if (now - lastMqttTry >= MQTT_RETRY_MS) {
    lastMqttTry = now;
    connectMqtt();
  }
#endif

  if (now - lastSerialLog >= SERIAL_LOG_INTERVAL_MS) {
    lastSerialLog = now;
    serialLog();
  }

  delay(5);   // feeds the SDK's housekeeping; without it the WiFi stack starves
}
