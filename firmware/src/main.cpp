// =========================================================
// Sentinel-X — NodeMCU LoLin V3 / ESP8266MOD (ESP-12F)
//
// An alarm box. Sensors in, three LEDs and a buzzer out, MQTT both ways.
//
// It hosts no access point and serves no web page: it joins the server's
// hotspot as an ordinary client and is watched and driven entirely from the
// main dashboard (../dashboard) over MQTT. Nothing listens on this board, so
// there is no surface on it to attack.
//
// Sensors:  DHT22 (D2), HC-SR501 PIR (D1), MQ gas (A0 via divider),
//           MPU-6500 tilt/shock/rotation (D6/D7)
// Outputs:  green / yellow / red LEDs, active buzzer
//
// Every tunable lives in include/config.h.
// =========================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <bearssl/bearssl_hash.h>

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
#if ENABLE_MQTT
  #include <PubSubClient.h>
  #if MQTT_USE_TLS
    #include <WiFiClientSecure.h>
  #endif
#endif

// =========================================================
// Pin sanity checks.
//
// static_assert and not #error: D0..D8 are "static const uint8_t" in the
// ESP8266 core rather than macros, so the preprocessor cannot compare them —
// it sees them as undefined and every comparison collapses to 0 == 0.
// =========================================================

#if ENABLE_LEDS
static_assert(PIN_LED_GREEN != PIN_LED_YELLOW && PIN_LED_GREEN != PIN_LED_RED
           && PIN_LED_YELLOW != PIN_LED_RED,
              "The three status LEDs must be on three different pins.");
static_assert(PIN_LED_RED == D8,
              "The red LED must be on D8/GPIO15, wired active-high, or the chip "
              "will not boot. See config.h section 2.");
#endif
#if ENABLE_BUZZER
static_assert(PIN_BUZZER == D4,
              "The buzzer must be on D4/GPIO2. See config.h section 2.");
static_assert(PIN_BUZZER != PIN_LED_GREEN && PIN_BUZZER != PIN_LED_YELLOW
           && PIN_BUZZER != PIN_LED_RED, "The buzzer collides with an LED pin.");
#endif
#if ENABLE_IMU
static_assert(PIN_IMU_SDA != PIN_IMU_SCL, "SDA and SCL cannot share a pin.");
#endif

// =========================================================
// Alarm state
//
// Modelled on a real panel. Only ARMED watches for intrusion; only a NEW
// detection starts a cycle, so a sensor stuck high raises one alarm rather
// than an endless siren.
// =========================================================

enum AlarmState : uint8_t {
  AL_EXIT_DELAY = 0,   // just armed: time to leave the room
  AL_ARMED      = 1,   // watching
  AL_ENTRY_DELAY= 2,   // something seen: identify yourself before it sounds
  AL_ALARM      = 3,   // nobody identified in time
  AL_MAINT      = 4,   // unlocked: silent, nothing alarms
};

static AlarmState alarmState = AL_EXIT_DELAY;
static unsigned long lastTriggerMs = 0;      // derniere detection, pour TRIGGER_RECENT_MS

// Ce que la camera voit. Alimente par le topic vision, perime au bout de
// VISION_STALE_MS pour qu'un service arrete ne laisse pas le boitier croire
// indefiniment qu'une personne autorisee est la.
static bool visionKnown = false;
static bool visionUnknown = false;
static char visionName[32] = "";
static unsigned long visionSeenMs = 0;

static bool visionFresh() {
  return visionSeenMs && (millis() - visionSeenMs) < VISION_STALE_MS;
}
// Une personne AUTORISEE est-elle visible en ce moment ?
static bool validatedPerson() { return visionFresh() && visionKnown; }
static unsigned long stateSince = 0;        // millis() when we entered it
static unsigned long stateDeadline = 0;     // when this state ends on its own
static char alarmCause[64] = "";            // what started the current cycle
static uint32_t alarmCount = 0;             // alarms since boot

// What the outputs are doing, as a PATTERN rather than a level: the dashboard
// polls far slower than a blink, so sampling a level would show a blinking LED
// as randomly on or off. Codes are shared with the dashboard's JavaScript.
enum OutPattern : uint8_t {
  PAT_OFF = 0, PAT_ON = 1, PAT_BLINK = 2, PAT_FAST = 3,
  PAT_BURST = 4, PAT_CHIRP = 5, PAT_TICK = 6, PAT_ALT = 7
};
static uint8_t ledPat[3] = {PAT_OFF, PAT_OFF, PAT_OFF};
static uint8_t buzzPat = PAT_OFF;
static const uint8_t LED_PINS[3] = {PIN_LED_GREEN, PIN_LED_YELLOW, PIN_LED_RED};

// Manual override of the outputs, maintenance only.
static bool manualMode = false;
static bool manLed[3] = {false, false, false};
static bool manBuzz = false;
static int8_t forcedState = -1;             // preview a state, maintenance only
static bool buzzerMuted = false;
static bool sniffEnabled = SNIFF_ON_AT_BOOT;   // bouton du dashboard pour l'allumer
static unsigned long ledOverrideUntil = 0;  // legacy {"led":...} command
static bool ledOverride[3] = {false, false, false};
static unsigned long buzzerOverrideUntil = 0;
static bool buzzerOverrideState = false;


// Forward declarations. This is a .cpp, so unlike an .ino the build generates
// no prototypes and definition order would otherwise dictate the layout.
static void publishAlert(const char *type, const char *level, const char *msg);
#if ENABLE_MQTT
static void publishState();
#endif
static void publishStateLocal();
static void buildStateJson();
static void buildTelemetryJson();
static void buildTestJson();
static void buildTestMetaJson();
static void enterState(AlarmState s, unsigned long holdMs, const char *cause);
static const char *alarmStateName(AlarmState s);
static bool inMaintenance();
static bool pirWarming();
static void newNonce();
static bool maintPinUsable();
static void leaveMaintenance(const char *why);
static bool selfTestRunning();
static void buzzerWrite(bool on);
static void ledWrite(uint8_t pin, bool on);
#if ENABLE_BUZZER
static bool burstOn(unsigned long t);
static bool tickOn(unsigned long t);
#endif
#if ENABLE_GAS
static float gasDelta();
static float gasCeiling();
static void gasRelearn();
#endif
#if ENABLE_IMU
static bool imuHasRef();
static void imuRelearn();
#endif

// =========================================================
// Sensor state
//
// The loop owns all sampling; the MQTT and serial paths only ever report the
// last values measured.
// =========================================================

#if ENABLE_CLIMATE
DHT dht(PIN_DHT, DHT_TYPE);
#endif
static float lastCelsius = NAN;
static float lastHumidity = NAN;
static bool  climateHealthy = false;

// --- gas --------------------------------------------------------------------
static int   lastGasRaw = 0;
static float lastGasVolts = NAN;
static float gasBaseline = NAN;
static float gasBaselineSum = 0;
static int   gasBaselineN = 0;
static bool  gasWarn = false;       // slow: above baseline
static bool  gasCrit = false;
static bool  gasRise = false;       // fast: sharp rise, no baseline needed
static float gasRiseAmount = 0;
static unsigned long gasStateUntil = 0;   // holds a gas state briefly after it clears

// Rolling minimum over GAS_RISE_WINDOW_MS, as a ring of samples.
static const uint8_t GAS_RING = (uint8_t)(GAS_RISE_WINDOW_MS / GAS_INTERVAL_MS);
static float gasRing[GAS_RING];
static uint8_t gasRingN = 0;
static uint8_t gasRingHead = 0;

// --- motion -----------------------------------------------------------------
static int  pirRaw = LOW;
static bool pirConfirmed = false;          // held PIR_CONFIRM_MS
static bool pirPrevConfirmed = false;
static unsigned long pirSince = 0;

// --- tamper -----------------------------------------------------------------
#if ENABLE_IMU
static ImuReading imu = {};
static uint8_t imuWho = 0;
static bool  imuOk = false;
static bool  imuTamper = false;
static bool  imuPrevTamper = false;
static float imuTilt = NAN;                // degrees from the armed orientation
static float imuShock = 0;                 // |a| departure from rest, in g
static float imuSpin = 0;                  // peak |gyro - bias|, deg/s
static float imuRefVec[3] = {NAN, NAN, NAN};
static float imuRestMag = NAN;
static float imuGyroBias[3] = {0, 0, 0};
static float imuRefSum[6] = {0, 0, 0, 0, 0, 0};
static uint8_t imuRefN = 0;
static unsigned long imuStillSince = 0;
#endif

// =========================================================
// Reference capture
// =========================================================

#if ENABLE_IMU
// Forget the armed orientation and measure it again.
static void imuRelearn() {
  for (uint8_t i = 0; i < 3; i++) imuRefVec[i] = NAN;
  for (uint8_t i = 0; i < 6; i++) imuRefSum[i] = 0;
  imuRefN = 0;
  imuRestMag = NAN;
  imuTamper = imuPrevTamper = false;
  imuTilt = NAN;
  imuShock = 0;
  imuSpin = 0;
}

static bool imuHasRef() { return !isnan(imuRefVec[0]); }
#endif

#if ENABLE_GAS
static float gasCeiling() { return ADC_FULL_SCALE_V * GAS_DIVIDER_RATIO; }

static float gasDelta() {
  if (isnan(gasBaseline) || isnan(lastGasVolts)) return NAN;
  return lastGasVolts - gasBaseline;
}

static void gasRelearn() {
  gasBaseline = NAN;
  gasBaselineSum = 0;
  gasBaselineN = 0;
  gasWarn = gasCrit = gasRise = false;
  gasRiseAmount = 0;
  gasRingN = gasRingHead = 0;
}
#endif

// =========================================================
// Reading the sensors
// =========================================================

#if ENABLE_CLIMATE
static void readClimate() {
  const float h = dht.readHumidity();
  const float c = dht.readTemperature();
  if (isnan(h) || isnan(c)) {
    climateHealthy = false;
    return;
  }
  lastHumidity = h;
  lastCelsius = c;
  climateHealthy = true;
}
#endif

#if ENABLE_GAS
static void readGas() {
  // Four samples, not eight: at 4 Hz the loop must stay responsive, and the
  // MQ's own noise is smoothed by the rise window anyway.
  long sum = 0;
  for (uint8_t i = 0; i < 4; i++) { sum += analogRead(PIN_GAS); delay(1); }
  lastGasRaw = (int)(sum / 4);
  lastGasVolts = (lastGasRaw * ADC_FULL_SCALE_V / ADC_MAX) * GAS_DIVIDER_RATIO;

  const unsigned long now = millis();

  // --- fast detector: a sharp rise above the lowest value in the window ---
  float windowMin = lastGasVolts;
  for (uint8_t i = 0; i < gasRingN; i++) {
    if (gasRing[i] < windowMin) windowMin = gasRing[i];
  }
  gasRiseAmount = lastGasVolts - windowMin;
  const bool riseNow = now > GAS_RISE_ARM_MS && gasRingN >= GAS_RING / 2
                    && gasRiseAmount >= GAS_RISE_V;

  gasRing[gasRingHead] = lastGasVolts;
  gasRingHead = (uint8_t)((gasRingHead + 1) % GAS_RING);
  if (gasRingN < GAS_RING) gasRingN++;

  // --- slow detector: settled baseline, then a delta above it ---
  bool warnNow = false, critNow = false;
  if (isnan(gasBaseline)) {
    if (now >= GAS_WARMUP_MS) {
      gasBaselineSum += lastGasVolts;
      if (++gasBaselineN >= GAS_BASELINE_SAMPLES) {
        gasBaseline = gasBaselineSum / gasBaselineN;
        Serial.printf("[gas] baseline %.2f V (headroom %.2f V)\n",
                      gasBaseline, gasCeiling() - gasBaseline);
        if (gasCeiling() - gasBaseline < GAS_CRIT_DELTA_V) {
          Serial.println(F("[gas] WARNING: little headroom left - the slow detector"));
          Serial.println(F("[gas]          may never reach its threshold. The fast"));
          Serial.println(F("[gas]          rise detector still works."));
        }
      }
    }
  } else {
    const float d = gasDelta();
    warnNow = d >= GAS_WARN_DELTA_V;
    critNow = d >= GAS_CRIT_DELTA_V;
    // Follow slow drift (heater, room temperature) but freeze the moment the
    // reading moves away, so a real leak is never learned as normal.
    if (!warnNow && !riseNow) {
      gasBaseline += (lastGasVolts - gasBaseline) * GAS_BASELINE_FOLLOW;
    }
  }

  // Hold a gas state briefly so a reading that dips back under the threshold
  // for one sample does not flicker the alarm.
  if (riseNow || critNow || warnNow) gasStateUntil = now + GAS_HOLD_MS;
  const bool holding = now < gasStateUntil;
  gasRise = riseNow || (gasRise && holding);
  gasCrit = critNow || (gasCrit && holding);
  gasWarn = warnNow || (gasWarn && holding);
}
#endif

static bool pirWarming() { return millis() < PIR_WARMUP_MS; }

#if ENABLE_MOTION
// The HC-SR501 holds its output high for as long as its own TIME knob says,
// and retriggers while anything moves - no firmware can shorten that. What we
// can do is require the level to HOLD before believing it, and act only on the
// rising edge, so a sensor stuck high is one event and not a continuous one.
static void pollMotion() {
  const int level = digitalRead(PIN_PIR);
  const unsigned long now = millis();
  if (level != pirRaw) {
    pirRaw = level;
    pirSince = now;
  }
  const bool held = now - pirSince >= PIR_CONFIRM_MS;
  if (held) pirConfirmed = pirRaw == HIGH;
}
#endif

#if ENABLE_IMU
static void readImu() {
  // Re-probe instead of giving up for good. imuBegin() resets and reconfigures
  // the part, so this also recovers a module that was unplugged and put back.
  if (!imuOk) {
    static unsigned long lastProbe = 0;
    const unsigned long now = millis();
    if (now - lastProbe < IMU_RETRY_MS) return;
    lastProbe = now;
    imuWho = imuBegin();
    imuOk = imuWho != 0;
    if (imuOk) {
      Serial.printf("[imu] trouve apres %lu s : WHO_AM_I=0x%02X\n", now / 1000, imuWho);
      imuRelearn();          // capture the reference from where it is now
    }
    return;
  }
  if (!imuRead(&imu)) return;

  const unsigned long now = millis();

  // Capture the resting orientation, |a| and gyro bias, averaged over a second.
  if (!imuHasRef()) {
    if (now < IMU_BASELINE_MS) return;
    imuRefSum[0] += imu.ax; imuRefSum[1] += imu.ay; imuRefSum[2] += imu.az;
    imuRefSum[3] += imu.gx; imuRefSum[4] += imu.gy; imuRefSum[5] += imu.gz;
    if (++imuRefN >= IMU_REF_SAMPLES) {
      for (uint8_t i = 0; i < 3; i++) {
        imuRefVec[i] = imuRefSum[i] / imuRefN;
        imuGyroBias[i] = imuRefSum[i + 3] / imuRefN;
      }
      imuRestMag = sqrtf(imuRefVec[0] * imuRefVec[0] + imuRefVec[1] * imuRefVec[1]
                       + imuRefVec[2] * imuRefVec[2]);
      Serial.printf("[imu] armed at rest: |a| %.2f g\n", imuRestMag);
    }
    return;
  }

  // Three independent signals; any one of them means the box was touched.
  imuTilt  = imuAngleFromRef(imu, imuRefVec);
  imuShock = fabsf(imu.magnitude - imuRestMag);
  imuSpin  = fmaxf(fmaxf(fabsf(imu.gx - imuGyroBias[0]), fabsf(imu.gy - imuGyroBias[1])),
                   fabsf(imu.gz - imuGyroBias[2]));

  const bool disturbed = imuTilt > TILT_WARN_DEG
                      || imuShock > IMU_SHOCK_DELTA_G
                      || imuSpin  > IMU_GYRO_DPS;

  if (disturbed) {
    imuStillSince = now;
    imuTamper = true;
  } else if (imuTamper && now - imuStillSince >= IMU_TAMPER_CLEAR_MS) {
    // Must be still for a while before we call it settled. A box set back down
    // at a new angle keeps reporting tamper, which is correct: it moved.
    imuTamper = imuTilt > TILT_WARN_DEG;
    if (!imuTamper) imuStillSince = now;
  }
}
#endif

// =========================================================
// Outputs
// =========================================================

static void ledWrite(uint8_t pin, bool on) {
#if ENABLE_LEDS
  digitalWrite(pin, on ? HIGH : LOW);     // all three LEDs are active-high
#else
  (void)pin; (void)on;
#endif
}

// Active-low vs active-high lives in one place, so rewiring is a config change.
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

#if ENABLE_BUZZER
// The rhythms. An active buzzer has one pitch and one loudness, so rhythm is
// the only way to tell states apart by ear.
static bool burstOn(unsigned long t) {
  const unsigned long beat  = BUZZER_RED_BEEP_MS + BUZZER_RED_GAP_MS;
  const unsigned long burst = beat * BUZZER_RED_BEEPS;
  t %= burst + BUZZER_RED_PAUSE_MS;
  return t < burst && (t % beat) < BUZZER_RED_BEEP_MS;
}
static bool chirpOn(unsigned long t) {
  return (t % BUZZER_YELLOW_PERIOD_MS) < BUZZER_YELLOW_ON_MS;
}
static bool tickOn(unsigned long t) {
  return (t % BUZZER_ENTRY_PERIOD_MS) < BUZZER_ENTRY_ON_MS;
}
#endif

// =========================================================
// The alarm state machine
// =========================================================

static bool earlyWarning = false;     // presence or gas warning, no alarm
static char warnReason[64] = "";

static const char *alarmStateName(AlarmState s) {
  switch (s) {
    case AL_EXIT_DELAY:  return "exit";
    case AL_ARMED:       return "armed";
    case AL_ENTRY_DELAY: return "entry";
    case AL_ALARM:       return "alarm";
    case AL_MAINT:       return "maintenance";
  }
  return "?";
}

static void enterState(AlarmState s, unsigned long holdMs, const char *cause) {
  alarmState = s;
  stateSince = millis();
  stateDeadline = holdMs ? stateSince + holdMs : 0;
  if (cause) {
    strncpy(alarmCause, cause, sizeof(alarmCause) - 1);
    alarmCause[sizeof(alarmCause) - 1] = '\0';
  }
  Serial.printf("[alarm] -> %s%s%s\n", alarmStateName(s),
                cause ? " : " : "", cause ? cause : "");
}

// Seconds left in a countdown, 0 when there is none. Published so the
// dashboard can show the same number the box is counting.
static uint16_t stateSecondsLeft() {
  if (!stateDeadline) return 0;
  const long left = (long)(stateDeadline - millis());
  return left > 0 ? (uint16_t)((left + 999) / 1000) : 0;
}

// Quelque chose est-il ENCORE en train d'etre detecte ?
//
// Utilise a l'expiration de la temporisation d'entree pour decider si on
// declenche vraiment. Le sabotage compte toujours : un boitier qu'on a deplace
// reste un sabotage meme immobile depuis.
static bool somethingStillDetected() {
#if ENABLE_IMU
  if (imuTamper) return true;              // exception : toujours
#endif
#if ENABLE_GAS
  if (gasCrit || gasRise) return true;
#endif
#if ENABLE_MOTION
  if (pirConfirmed) return true;
#endif
  if (visionFresh() && visionUnknown) return true;
  // Le PIR retombe entre deux passages : une detection tres recente compte.
  return lastTriggerMs && (millis() - lastTriggerMs) < TRIGGER_RECENT_MS;
}

// Decides what the box has seen, and moves the state machine.
static void evaluateAlarm() {
  const unsigned long now = millis();

  // --- what the sensors say right now ---
  bool intrusion = false;             // a NEW confirmed detection
  const char *cause = nullptr;

#if ENABLE_MOTION
  // Rising edge only: a PIR stuck high must not restart the cycle forever.
  const bool pirRising = pirConfirmed && !pirPrevConfirmed && now > PIR_WARMUP_MS;
  pirPrevConfirmed = pirConfirmed;
  if (pirRising) { intrusion = true; cause = "Mouvement confirme (PIR)"; }
#endif

#if ENABLE_IMU
  const bool tamperRising = imuTamper && !imuPrevTamper;
  imuPrevTamper = imuTamper;
  if (tamperRising && imuHasRef()) {
    intrusion = true;
    cause = imuTilt > TILT_WARN_DEG ? "Boitier incline"
          : imuSpin > IMU_GYRO_DPS  ? "Boitier tourne" : "Boitier choque";
  }
#endif

  // --- early warning: present but not proof of an intruder ---
  earlyWarning = false;
  warnReason[0] = '\0';
#if ENABLE_PRESENCE
  if (presenceStats().warn) {
    earlyWarning = true;
    snprintf(warnReason, sizeof(warnReason), "Presence WiFi inhabituelle (+%.0f appareils)",
             (double)presenceStats().excess);
  }
#endif
#if ENABLE_GAS
  if (!earlyWarning && gasWarn && !gasCrit) {
    earlyWarning = true;
    snprintf(warnReason, sizeof(warnReason), "Gaz %+.2f V au-dessus de la baseline",
             (double)gasDelta());
  }
#endif

  // --- gas is a safety hazard, not an intrusion: straight to alarm ---
#if ENABLE_GAS
  const bool gasAlarm = gasCrit || gasRise;
  if (gasAlarm && alarmState != AL_ALARM) {
    const bool silenced = alarmState == AL_MAINT && MAINT_SILENCES_GAS;
    if (!silenced) {
      char why[64];
      if (gasRise) snprintf(why, sizeof(why), "Gaz : hausse de %.2f V", (double)gasRiseAmount);
      else         snprintf(why, sizeof(why), "Gaz : %+.2f V au-dessus de la baseline",
                            (double)gasDelta());
      alarmCount++;
      enterState(AL_ALARM, ALARM_DURATION_MS, why);
      return;
    }
  }
#endif

  // Horodate chaque detection, pour que TRIGGER_RECENT_MS ait une reference.
  // Le PIR confirme compte aussi, pas seulement son front montant : quelqu'un
  // qui reste immobile devant le capteur ne produit qu'un seul front.
  if (intrusion) lastTriggerMs = now;
#if ENABLE_MOTION
  if (pirConfirmed) lastTriggerMs = now;
#endif

  // --- ce que la camera voit ----------------------------------------------
  //
  // REGLE 1 : une personne autorisee en vue annule toute alarme d'intrusion,
  // en cours comme a venir. Le gaz n'est PAS concerne : etre reconnu ne rend
  // pas une fuite inoffensive.
  if (validatedPerson()) {
    if (alarmState == AL_ENTRY_DELAY || alarmState == AL_ALARM) {
      char why[64];
      snprintf(why, sizeof(why), "Valide : %s reconnu",
               visionName[0] ? visionName : "personne autorisee");
      publishAlert("validation", "info", why);
      enterState(AL_ARMED, 0, why);
    }
  }
  // REGLE 2 : un visage inconnu a VISION_IDENTIFY_MS pour se faire
  // reconnaitre. Il suffit de se presenter face a l'objectif.
  else if (visionFresh() && visionUnknown && alarmState == AL_ARMED) {
    enterState(AL_ENTRY_DELAY, VISION_IDENTIFY_MS,
               "Visage inconnu : montrez votre visage a la camera");
  }

  // --- the state machine proper ---
  switch (alarmState) {
    case AL_MAINT:
      // Nothing alarms here. It ends on its own so the box is never left
      // disarmed by accident.
      if (stateDeadline && (long)(now - stateDeadline) >= 0) {
        enterState(AL_EXIT_DELAY, EXIT_DELAY_MS, "Maintenance expiree");
      }
      break;

    case AL_EXIT_DELAY:
      if ((long)(now - stateDeadline) >= 0) enterState(AL_ARMED, 0, "Arme");
      break;

    case AL_ARMED:
      // Pas de nouveau cycle tant qu'une personne autorisee est devant la
      // camera : son mouvement, et le boitier qu'elle manipule, sont normaux.
      if (intrusion && !validatedPerson()) {
        enterState(AL_ENTRY_DELAY, ENTRY_DELAY_MS, cause);
      }
      break;

    case AL_ENTRY_DELAY:
      if ((long)(now - stateDeadline) >= 0) {
#if ALARM_NEEDS_LIVE_TRIGGER
        if (!somethingStillDetected()) {
          // Plus rien : la personne est repartie, ou c'etait un passage. On
          // re-arme sans sirene, et on le dit, pour que ce ne soit pas pris
          // pour un capteur mort.
          publishAlert("fausse-alerte", "info",
                       "Temporisation ecoulee, plus rien detecte : re-armement");
          enterState(AL_ARMED, 0, "Re-arme : plus rien detecte");
          break;
        }
#endif
        alarmCount++;
        enterState(AL_ALARM, ALARM_DURATION_MS, alarmCause);
      }
      break;

    case AL_ALARM:
      // Re-arms on its own. A still-present intruder trips it again from
      // ARMED, which is one new cycle rather than a siren that never stops.
      if ((long)(now - stateDeadline) >= 0) {
        enterState(AL_EXIT_DELAY, EXIT_DELAY_MS, "Fin d'alarme");
      }
      break;
  }
}

// =========================================================
// Driving the LEDs and buzzer from the state
// =========================================================

static bool sensorsHealthy() {
#if ENABLE_CLIMATE
  if (!climateHealthy) return false;
#endif
#if ENABLE_IMU
  if (!imuOk) return false;
#endif
  return true;
}

static void setLeds(bool g, bool y, bool r, uint8_t pg, uint8_t py, uint8_t pr) {
  ledWrite(PIN_LED_GREEN, g);   ledPat[0] = pg;
  ledWrite(PIN_LED_YELLOW, y);  ledPat[1] = py;
  ledWrite(PIN_LED_RED, r);     ledPat[2] = pr;
}

static void applyOutputs() {
  if (selfTestRunning()) return;            // the self-test owns the outputs

  const unsigned long now = millis();
  const bool slow = (now / 500) % 2;
  const bool fast = (now / 150) % 2;
  const bool alt  = (now / 700) % 2;

  // --- manual override, maintenance only ---
  if (manualMode && alarmState == AL_MAINT) {
    for (uint8_t i = 0; i < 3; i++) {
      ledWrite(LED_PINS[i], manLed[i]);
      ledPat[i] = manLed[i] ? PAT_ON : PAT_OFF;
    }
    const bool b = manBuzz || (now < buzzerOverrideUntil && buzzerOverrideState);
    buzzerWrite(b);
    buzzPat = b ? PAT_ON : PAT_OFF;
    return;
  }

  // --- legacy {"led":"rouge"} override, maintenance only, times out ---
  if (now < ledOverrideUntil && alarmState == AL_MAINT) {
    for (uint8_t i = 0; i < 3; i++) {
      ledWrite(LED_PINS[i], ledOverride[i]);
      ledPat[i] = ledOverride[i] ? PAT_ON : PAT_OFF;
    }
  } else {
    const AlarmState shown = forcedState >= 0 && alarmState == AL_MAINT
                           ? (AlarmState)forcedState : alarmState;
    switch (shown) {
      case AL_MAINT:
        // Unmistakably "not watching": green and yellow alternating.
        setLeds(alt, !alt, false, PAT_ALT, PAT_ALT, PAT_OFF);
        break;
      case AL_ALARM:
        setLeds(false, false, fast, PAT_OFF, PAT_OFF, PAT_FAST);
        break;
      case AL_ENTRY_DELAY:
        setLeds(false, fast, false, PAT_OFF, PAT_FAST, PAT_OFF);
        break;
      case AL_EXIT_DELAY:
        setLeds(false, slow, false, PAT_OFF, PAT_BLINK, PAT_OFF);
        break;
      case AL_ARMED:
      default:
        if (earlyWarning) {
          setLeds(false, slow, false, PAT_OFF, PAT_BLINK, PAT_OFF);
        } else {
          // Solid green = validated. Blinking green = running, but a sensor is
          // not answering: visible across the room without the dashboard.
          const bool ok = sensorsHealthy();
          setLeds(ok ? true : slow, false, false, ok ? PAT_ON : PAT_BLINK,
                  PAT_OFF, PAT_OFF);
        }
        break;
    }
  }

  // --- buzzer ---
#if ENABLE_BUZZER
  if (now < buzzerOverrideUntil) {
    buzzerWrite(buzzerOverrideState);
    buzzPat = buzzerOverrideState ? PAT_ON : PAT_OFF;
    return;
  }
  if (buzzerMuted || alarmState == AL_MAINT) {
    buzzerWrite(false);
    buzzPat = PAT_OFF;
    return;
  }
  switch (alarmState) {
    case AL_ALARM:
      buzzerWrite(burstOn(now)); buzzPat = PAT_BURST; break;
    case AL_ENTRY_DELAY:
    case AL_EXIT_DELAY:
      buzzerWrite(tickOn(now)); buzzPat = PAT_TICK; break;
    default:
      if (earlyWarning && BUZZER_EARLY_WARNING) {
        buzzerWrite(chirpOn(now)); buzzPat = PAT_CHIRP;
      } else {
        buzzerWrite(false); buzzPat = PAT_OFF;
      }
      break;
  }
#endif
}

// =========================================================
// Maintenance mode and its lock
//
// Maintenance is the one unlocked state: silent, nothing alarms, and the
// dashboard may drive the outputs, preview states, run the self-test and
// recalibrate. Everything in that list is refused by the BOARD outside
// maintenance, not merely greyed out on the page.
//
// Getting in needs the PIN, proven without ever sending it:
//
//   1. the board publishes a fresh random nonce in its state;
//   2. the dashboard sends sha256(nonce + ":" + PIN) as hex;
//   3. the board recomputes and compares in constant time.
//
// The nonce is single-use - replaced after every attempt, right or wrong - so
// a captured answer cannot be replayed. Only the board may subscribe to the
// cmd topic (Mosquitto ACL), so answers cannot be harvested for an offline
// guess either.
// =========================================================

static char     maintNonce[17] = "";      // 8 random bytes as hex
static uint8_t  maintFails = 0;
static unsigned long maintLockedUntil = 0;
static uint32_t maintGrants = 0;
static uint32_t maintDenials = 0;

// A PIN left at the template value, or too short, disables remote maintenance
// entirely rather than shipping a guessable lock.
static bool maintPinUsable() {
  const size_t n = strlen(MAINT_PIN);
  return n >= MAINT_PIN_MIN_LEN && strcmp(MAINT_PIN, "change-me") != 0;
}

static void newNonce() {
  static const char hex[] = "0123456789abcdef";
  for (uint8_t i = 0; i < 8; i++) {
    const uint8_t b = (uint8_t)(RANDOM_REG32 & 0xFF);
    maintNonce[i * 2]     = hex[b >> 4];
    maintNonce[i * 2 + 1] = hex[b & 0x0F];
  }
  maintNonce[16] = '\0';
}

static void sha256Hex(const char *in, char out[65]) {
  uint8_t digest[32];
  br_sha256_context ctx;
  br_sha256_init(&ctx);
  br_sha256_update(&ctx, in, strlen(in));
  br_sha256_out(&ctx, digest);
  static const char hex[] = "0123456789abcdef";
  for (uint8_t i = 0; i < 32; i++) {
    out[i * 2]     = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 0x0F];
  }
  out[64] = '\0';
}

// Compares every byte regardless of where they differ, so how long the check
// takes says nothing about how much of the answer was right.
static bool constantTimeEqual(const char *a, const char *b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
  return diff == 0;
}

// Returns nullptr on success, or a reason the dashboard shows as-is.
static const char *maintTry(const char *answer) {
  const unsigned long now = millis();

  if (!maintPinUsable()) {
    return "PIN de maintenance non configure sur le boitier";
  }
  if (maintLockedUntil && (long)(now - maintLockedUntil) < 0) {
    return "Trop d'essais : verrouille";
  }
  if (!answer || strlen(answer) != 64) {
    newNonce();
    return "Reponse invalide";
  }

  char expect[65];
  char material[16 + 1 + 64 + 1];
  snprintf(material, sizeof(material), "%s:%s", maintNonce, MAINT_PIN);
  sha256Hex(material, expect);
  memset(material, 0, sizeof(material));     // do not leave the PIN in RAM

  const bool ok = constantTimeEqual(expect, answer, 64);
  newNonce();                                 // single use, pass or fail

  if (!ok) {
    maintDenials++;
    if (++maintFails >= MAINT_MAX_FAILS) {
      maintLockedUntil = now + MAINT_LOCKOUT_MS;
      maintFails = 0;
      publishAlert("securite", "critique",
                   "Maintenance verrouillee apres trop d'essais");
      return "Trop d'essais : verrouille";
    }
    publishAlert("securite", "attention", "Echec d'authentification maintenance");
    return "PIN incorrect";
  }

  maintFails = 0;
  maintLockedUntil = 0;
  maintGrants++;
  enterState(AL_MAINT, MAINT_TIMEOUT_MS, "Maintenance");
  publishAlert("maintenance", "info", "Mode maintenance active");
  return nullptr;
}

// Everything unlocked by maintenance is undone on the way out, so the box
// cannot be left half-disarmed.
static void leaveMaintenance(const char *why) {
  manualMode = false;
  manLed[0] = manLed[1] = manLed[2] = false;
  manBuzz = false;
  forcedState = -1;
  buzzerMuted = false;
  sniffEnabled = true;
  ledOverrideUntil = 0;
  buzzerOverrideUntil = 0;
  enterState(AL_EXIT_DELAY, EXIT_DELAY_MS, why ? why : "Re-arme");
  publishAlert("maintenance", "info", "Mode maintenance termine");
}

static bool inMaintenance() { return alarmState == AL_MAINT; }

// =========================================================
// Self-test
//
// The same checks the standalone diagnostic sketches do, but inside the real
// firmware so they can be run from the dashboard without a cable.
//
// A non-blocking state machine, not a sequence of delay() calls: blocking here
// would stall MQTT, and the dashboard driving the test would lose the board
// mid-run. Maintenance only - it seizes the LEDs and buzzer.
// =========================================================

enum StepKind : uint8_t {
  SK_PIN, SK_BUZZ_PIN, SK_SOLO, SK_ALL_OFF, SK_ALL_ON,
  SK_BUZZ_ALARM, SK_BUZZ_TICK, SK_BUZZ_SILENT,
  SK_DHT, SK_IMU, SK_GAS, SK_PIR
};

// Shared with the dashboard's JavaScript. Keep the two in step.
enum StepResult : uint8_t {
  RES_PENDING = 0, RES_RUNNING = 1, RES_PASS = 2, RES_FAIL = 3,
  RES_INFO = 4, RES_LOOK = 5, RES_SKIP = 6
};

struct TestStep {
  const char *name;
  const char *hint;
  StepKind    kind;
  uint8_t     arg;
  uint16_t    ms;
  bool        visual;        // only a human can judge it
};

static const TestStep TEST_STEPS[] = {
  { "Broche LED verte",   "Controle electrique D5 / GPIO14",     SK_PIN,        0,  700, false },
  { "Broche LED jaune",   "Controle electrique D0 / GPIO16",     SK_PIN,        1,  700, false },
  { "Broche LED rouge",   "Controle electrique D8 / GPIO15",     SK_PIN,        2,  700, false },
  { "Broche buzzer",      "Controle electrique D4 / GPIO2",      SK_BUZZ_PIN,   0,  700, false },
  { "Verte seule",        "Seule la VERTE doit etre allumee",    SK_SOLO,       0, 2500, true  },
  { "Jaune seule",        "Seule la JAUNE doit etre allumee",    SK_SOLO,       1, 2500, true  },
  { "Rouge seule",        "Seule la ROUGE doit etre allumee",    SK_SOLO,       2, 2500, true  },
  { "Toutes eteintes",    "Les trois LED doivent etre eteintes", SK_ALL_OFF,    0, 2500, true  },
  { "Toutes allumees",    "Les trois LED doivent etre allumees", SK_ALL_ON,     0, 2500, true  },
  { "Buzzer : alarme",    "Trois bips, une pause, et ainsi de suite", SK_BUZZ_ALARM, 0, 4500, true },
  { "Buzzer : decompte",  "Un bip court chaque seconde",         SK_BUZZ_TICK,  0, 4000, true  },
  { "Buzzer : silence",   "Rien ne doit sonner",                 SK_BUZZ_SILENT,0, 2500, true  },
  { "DHT22",              "Derniere lecture temperature/humidite", SK_DHT,      0,  800, false },
  { "MPU-6500",           "Identite et orientation de reference", SK_IMU,       0,  800, false },
  { "Gaz MQ",             "Niveau A0, baseline et marge",        SK_GAS,        0,  800, false },
  { "PIR",                "Passez la main devant le capteur",    SK_PIR,        0, 8000, false },
};
static const uint8_t TEST_COUNT = sizeof(TEST_STEPS) / sizeof(TEST_STEPS[0]);

static bool     testActive = false;
static bool     testPaused = false;
static bool     testLoop = false;
static bool     testWaitConfirm = true;
static bool     testFinished = false;
static uint8_t  testStep = 0;
static unsigned long testStepStart = 0;
static unsigned long testPausedAt = 0;
static uint16_t testStepMs = 0;
static uint32_t testPasses = 0;
static uint8_t  testResult[TEST_COUNT];
static char     testDetail[TEST_COUNT][56];
static int      testPirStart = -1;
static bool     testPirChanged = false;
static bool     testDirty = true;          // republish the test topic

static bool selfTestRunning() { return testActive; }

static void testNote(uint8_t result, const char *fmt, ...) {
  testResult[testStep] = result;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(testDetail[testStep], sizeof(testDetail[0]), fmt, ap);
  va_end(ap);
  testDirty = true;
}

static void testSetLeds(bool g, bool y, bool r) {
  const bool on[3] = {g, y, r};
  for (uint8_t i = 0; i < 3; i++) {
    ledWrite(LED_PINS[i], on[i]);
    ledPat[i] = on[i] ? PAT_ON : PAT_OFF;
  }
}

static void testCheckLedPin(uint8_t i) {
#if ENABLE_LEDS
  const uint8_t pin = LED_PINS[i];
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH); delayMicroseconds(400);
  const int hi = digitalRead(pin);
  digitalWrite(pin, LOW);  delayMicroseconds(400);
  const int lo = digitalRead(pin);

  // The pull-up test only means something on GPIO14. GPIO16 has no internal
  // pull-up at all and GPIO15 carries an onboard pull-down, so running it
  // there would report a fault on a perfectly good board.
  int pu = -1;
  if (pin != D0 && pin != D8) {
    pinMode(pin, INPUT_PULLUP); delay(2);
    pu = digitalRead(pin);
    pinMode(pin, OUTPUT); digitalWrite(pin, LOW);
  }
  const bool ok = hi == HIGH && lo == LOW && pu != LOW;
  testNote(ok ? RES_PASS : RES_FAIL, "HIGH lit %s, LOW lit %s%s",
           hi ? "HIGH" : "LOW", lo ? "HIGH" : "LOW",
           pu < 0 ? "" : (pu ? ", pull-up ok" : ", TENU BAS"));
#else
  (void)i; testNote(RES_SKIP, "LED desactivees dans ce build");
#endif
}

static void testCheckBuzzerPin() {
#if ENABLE_BUZZER
  pinMode(PIN_BUZZER, OUTPUT);
  buzzerWrite(false); delayMicroseconds(400);
  const int offLvl = digitalRead(PIN_BUZZER);
  buzzerWrite(true);  delayMicroseconds(400);    // 400 us: an inaudible click
  const int onLvl = digitalRead(PIN_BUZZER);
  buzzerWrite(false);
  const int wantOn = BUZZER_ACTIVE_LOW ? LOW : HIGH;
  const bool ok = onLvl == wantOn && offLvl != wantOn;
  testNote(ok ? RES_PASS : RES_FAIL, "silence %s, son %s (actif %s)",
           offLvl ? "HIGH" : "LOW", onLvl ? "HIGH" : "LOW",
           BUZZER_ACTIVE_LOW ? "bas" : "haut");
#else
  testNote(RES_SKIP, "buzzer desactive dans ce build");
#endif
}

static void testStartStep(uint8_t i) {
  testStep = i;
  testStepStart = millis();
  testPausedAt = 0;
  testResult[i] = RES_RUNNING;
  testDetail[i][0] = '\0';
  testDirty = true;

  const TestStep &s = TEST_STEPS[i];
  testStepMs = s.ms;
  testSetLeds(false, false, false);
  buzzerWrite(false); buzzPat = PAT_OFF;

  switch (s.kind) {
    case SK_PIN:      testCheckLedPin(s.arg); break;
    case SK_BUZZ_PIN: testCheckBuzzerPin(); break;
    case SK_SOLO:     testSetLeds(s.arg == 0, s.arg == 1, s.arg == 2); break;
    case SK_ALL_ON:   testSetLeds(true, true, true); break;
    case SK_ALL_OFF:  break;
    case SK_BUZZ_ALARM:  buzzPat = PAT_BURST; break;
    case SK_BUZZ_TICK:   buzzPat = PAT_TICK;  break;
    case SK_BUZZ_SILENT: buzzPat = PAT_OFF;   break;

    case SK_DHT:
#if ENABLE_CLIMATE
      if (climateHealthy) testNote(RES_PASS, "%.1f C, %.0f %%HR", lastCelsius, lastHumidity);
      else testNote(RES_FAIL, "aucune reponse : VCC en 3,3 V et DATA sur D2 ?");
#else
      testNote(RES_SKIP, "climat desactive dans ce build");
#endif
      break;

    case SK_IMU:
#if ENABLE_IMU
      if (!imuOk)            testNote(RES_FAIL, "aucune reponse sur D6/D7");
      else if (!imuHasRef()) testNote(RES_INFO, "0x%02X, reference en cours", imuWho);
      else                   testNote(RES_PASS, "0x%02X, %.1f deg de la reference%s",
                                      imuWho, imuTilt, imuTamper ? " - SABOTAGE" : "");
#else
      testNote(RES_SKIP, "IMU desactive dans ce build");
#endif
      break;

    case SK_GAS:
#if ENABLE_GAS
      if (lastGasRaw <= 2)        testNote(RES_FAIL, "colle a 0 : pas d'alim, ou AO en court-circuit");
      else if (lastGasRaw >= 1020) testNote(RES_FAIL, "sature : verifier le pont diviseur");
      else if (isnan(gasBaseline)) testNote(RES_INFO, "%.2f V, baseline pas encore prise", lastGasVolts);
      else                         testNote(RES_PASS, "%.2f V, ecart %+.2f, marge %.2f V",
                                            lastGasVolts, gasDelta(), gasCeiling() - gasBaseline);
#else
      testNote(RES_SKIP, "gaz desactive dans ce build");
#endif
      break;

    case SK_PIR:
#if ENABLE_MOTION
      testPirStart = digitalRead(PIN_PIR);
      testPirChanged = false;
      testNote(RES_RUNNING, "en attente d'un changement d'etat...");
#else
      testNote(RES_SKIP, "mouvement desactive dans ce build");
#endif
      break;
  }

  if (testResult[i] == RES_SKIP) testStepMs = 300;
}

static void testEnd(bool completed) {
  if (!completed && testActive && testResult[testStep] == RES_RUNNING) {
    testNote(RES_SKIP, "arrete");
  }
  testActive = false;
  testPaused = false;
  testFinished = completed;
  testSetLeds(false, false, false);
  buzzerWrite(false); buzzPat = PAT_OFF;
  testDirty = true;
  Serial.println(completed ? F("[test] termine") : F("[test] arrete"));
}

static void testMoveOn() {
  buzzerWrite(false);
  if (testStep + 1 < TEST_COUNT) { testStartStep(testStep + 1); return; }
  testPasses++;
  if (testLoop) { testStartStep(0); return; }
  testEnd(true);
}

static void testFinishStep() {
  const TestStep &s = TEST_STEPS[testStep];
#if ENABLE_MOTION
  if (s.kind == SK_PIR && testResult[testStep] == RES_RUNNING) {
    if (testPirChanged)      testNote(RES_PASS, "changement d'etat : capteur vivant");
    else if (pirWarming())   testNote(RES_INFO, "aucun changement, encore en prechauffage");
    else                     testNote(RES_INFO, "aucun changement : bloque %s, ou personne n'a bouge",
                                      testPirStart ? "HAUT" : "BAS");
  }
#endif
  if (testResult[testStep] == RES_RUNNING) {
    testResult[testStep] = s.visual ? RES_LOOK : RES_INFO;
    testDirty = true;
  }
}

static void testStart() {
  for (uint8_t k = 0; k < TEST_COUNT; k++) {
    testResult[k] = RES_PENDING;
    testDetail[k][0] = '\0';
  }
  testPasses = 0;
  testFinished = false;
  testActive = true;
  testPaused = false;
  forcedState = -1;
  manualMode = false;
  Serial.println(F("[test] demarre"));
  testStartStep(0);
}

static void testTick() {
  if (!testActive || testPaused) return;
  const TestStep &s = TEST_STEPS[testStep];
  const unsigned long elapsed = millis() - testStepStart;

#if ENABLE_BUZZER
  if (testResult[testStep] != RES_SKIP) {
    if (s.kind == SK_BUZZ_ALARM) buzzerWrite(burstOn(elapsed));
    if (s.kind == SK_BUZZ_TICK)  buzzerWrite(tickOn(elapsed));
  }
#endif
#if ENABLE_MOTION
  if (s.kind == SK_PIR && digitalRead(PIN_PIR) != testPirStart) testPirChanged = true;
#endif

  if (elapsed < testStepMs) return;

  // Guided mode holds an unanswered visual step - outputs still running - until
  // someone says whether it looked right.
  if (testWaitConfirm && s.visual && testResult[testStep] == RES_RUNNING) return;

  testFinishStep();
  testMoveOn();
}

static bool testConfirm(long step, bool ok) {
  if (step < 0 || step >= TEST_COUNT || !TEST_STEPS[step].visual) return false;
  const uint8_t r = testResult[step];
  if (r == RES_PENDING || r == RES_SKIP) return false;
  testResult[step] = ok ? RES_PASS : RES_FAIL;
  snprintf(testDetail[step], sizeof(testDetail[0]),
           ok ? "confirme a l'oeil / a l'oreille" : "signale INCORRECT");
  testDirty = true;
  if (testActive && !testPaused && (uint8_t)step == testStep && testWaitConfirm) testMoveOn();
  return true;
}

// Returns nullptr on success, or a reason the dashboard shows as-is.
static const char *testAction(const char *a, long step, long flag) {
  if (!inMaintenance()) return "Passez en maintenance d'abord";

  if      (!strcmp(a, "start"))  testStart();
  else if (!strcmp(a, "stop"))   { if (testActive) testEnd(false); }
  else if (!strcmp(a, "pause"))  { if (testActive && !testPaused) { testPausedAt = millis() - testStepStart; testPaused = true; buzzerWrite(false); } }
  else if (!strcmp(a, "resume")) { if (testActive && testPaused) { testStepStart = millis() - testPausedAt; testPaused = false; } }
  else if (!strcmp(a, "toggle")) { return testAction(testPaused ? "resume" : "pause", step, flag); }
  else if (!strcmp(a, "next"))   { if (!testActive) return "Test non demarre";
                                   if (testResult[testStep] == RES_RUNNING) testNote(RES_SKIP, "saute");
                                   testPaused = false; testMoveOn(); }
  else if (!strcmp(a, "prev"))   { if (!testActive) return "Test non demarre";
                                   testPaused = false;
                                   testStartStep(testStep > 0 ? testStep - 1 : 0); }
  else if (!strcmp(a, "goto"))   { if (step < 0 || step >= TEST_COUNT) return "Etape hors limites";
                                   if (!testActive) testStart();
                                   testPaused = false; testStartStep((uint8_t)step); }
  else if (!strcmp(a, "loop"))   testLoop = flag != 0;
  else if (!strcmp(a, "wait"))   testWaitConfirm = flag != 0;
  else if (!strcmp(a, "confirm")) { if (!testConfirm(step, flag != 0)) return "Cette etape n'attend pas de reponse"; }
  else return "Action de test inconnue";

  testDirty = true;
  return nullptr;
}

// =========================================================
// MQTT: the only way in and out
// =========================================================

#if ENABLE_MQTT
  #if MQTT_USE_TLS
BearSSL::WiFiClientSecure netClient;
BearSSL::X509List caCertList(CA_CERT);
  #else
WiFiClient netClient;
  #endif
PubSubClient mqtt(netClient);
#endif

static bool stateDirty = true;
#if ENABLE_MQTT
static unsigned long lastTelemetry = 0;
static unsigned long lastStatePub = 0;
static unsigned long mqttRetryAt = 0;
static unsigned long mqttBackoff = MQTT_RETRY_MIN_MS;
static bool testMetaSent = false;
#endif

// Per-second command budget. A flood on the cmd topic cannot make the board
// spend all its time parsing instead of watching its sensors.
#if ENABLE_MQTT
static unsigned long cmdWindowStart = 0;
static uint8_t cmdThisSecond = 0;
#endif
static uint32_t cmdDropped = 0;

// One shared buffer: only one message is ever being built at a time, and a
// static buffer keeps ~1.5 kB off the 4 kB stack.
//
// Sized from measurements, not guesswork: state is ~1 kB and the self-test
// step list ~1.5 kB. The first version used 1024 and silently truncated the
// step list into invalid JSON, so jTrunc now records an overflow and the
// publishers REFUSE to send a half-built message. A missing update is
// obvious; a malformed one looks like a dashboard bug.
static char   jbuf[1792];
static size_t jlen = 0;
static bool   jTrunc = false;

static void jReset() { jlen = 0; jbuf[0] = '\0'; jTrunc = false; }

static void jAdd(const char *fmt, ...) {
  if (jlen >= sizeof(jbuf) - 1) { jTrunc = true; return; }
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(jbuf + jlen, sizeof(jbuf) - jlen, fmt, ap);
  va_end(ap);
  if (n < 0) { jTrunc = true; return; }
  if ((size_t)n >= sizeof(jbuf) - jlen) {      // vsnprintf cut the tail off
    jlen = sizeof(jbuf) - 1;
    jTrunc = true;
    return;
  }
  jlen += n;
}

// Publishes only a complete message, and says so loudly when it cannot.
#if ENABLE_MQTT
static bool jPublish(const char *topic, bool retained) {
  if (jTrunc) {
    Serial.printf("[json] %s TRONQUE a %u octets - non publie. Agrandir jbuf.\n",
                  topic, (unsigned)jlen);
    return false;
  }
  if (!mqtt.publish(topic, jbuf, retained)) {
    Serial.printf("[mqtt] publication refusee sur %s (%u octets) - "
                  "MQTT_MAX_PACKET_SIZE trop petit ?\n", topic, (unsigned)jlen);
    return false;
  }
  return true;
}
#endif

static const char *bs(bool b) { return b ? "true" : "false"; }

static void fmtFloat(char *out, size_t n, float v, int dp) {
  if (isnan(v)) { strncpy(out, "null", n); out[n - 1] = '\0'; return; }
  dtostrf(v, 0, dp, out);
}

// Escapes the few characters that would otherwise break the JSON. Reasons are
// built from our own format strings, but they carry sensor numbers, so this
// stays cheap insurance rather than an assumption.
static void jString(const char *s) {
  jAdd("\"");
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') jAdd("\\%c", *s);
    else if ((uint8_t)*s >= 0x20) jAdd("%c", *s);
  }
  jAdd("\"");
}

static void publishAlert(const char *type, const char *level, const char *msg) {
#if ENABLE_MQTT
  if (!mqtt.connected()) return;
  jReset();
  jAdd("{\"type\":"); jString(type);
  jAdd(",\"niveau\":"); jString(level);
  jAdd(",\"msg\":"); jString(msg);
  jAdd(",\"source\":\"esp8266\"}");
  jPublish(TOPIC_ALERTS, false);
  Serial.printf("[alert] %s\n", jbuf);
#else
  (void)type; (void)level; (void)msg;
#endif
}

// Sensor readings, for the charts, the database and the AI. Unchanged in shape
// from what the API and the predictive model already expect.
static void buildTelemetryJson() {
  char t[12], h[12], gd[12];
  fmtFloat(t, sizeof(t), lastCelsius, 1);
  fmtFloat(h, sizeof(h), lastHumidity, 0);
  fmtFloat(gd, sizeof(gd), gasDelta(), 2);

  jReset();
  jAdd("{\"t\":%s,\"h\":%s,\"gaz\":%d,\"pir\":%d,\"rssi\":%d",
       t, h, lastGasRaw, pirConfirmed ? 1 : 0, (int)WiFi.RSSI());
  jAdd(",\"gaz_v\":%.2f,\"gaz_d\":%s", isnan(lastGasVolts) ? 0.0f : lastGasVolts, gd);
#if ENABLE_PRESENCE
  jAdd(",\"pres\":%u,\"sniff\":%u", (unsigned)presenceStats().count,
       (unsigned)presenceStats().count);
#endif
#if ENABLE_IMU
  jAdd(",\"tilt\":%d", imuTamper ? 1 : 0);
#endif
  jAdd(",\"state\":"); jString(alarmStateName(alarmState));
  jAdd(",\"heap\":%u}", (unsigned)ESP.getFreeHeap());
}

#if ENABLE_MQTT
static void publishTelemetry() {
  if (!mqtt.connected()) return;
  buildTelemetryJson();
  jPublish(TOPIC_TELEMETRY, false);
}
#endif

// Everything the dashboard needs to render the box. Retained, so a dashboard
// opened later sees the current state at once instead of a blank page.
static void buildStateJson() {
  char tilt[12], shock[12], gb[12], gdv[12];
  fmtFloat(gb, sizeof(gb), gasBaseline, 2);
  fmtFloat(gdv, sizeof(gdv), gasDelta(), 2);
#if ENABLE_IMU
  fmtFloat(tilt, sizeof(tilt), imuHasRef() ? imuTilt : NAN, 1);
  fmtFloat(shock, sizeof(shock), imuHasRef() ? imuShock : NAN, 2);
#else
  strcpy(tilt, "null"); strcpy(shock, "null");
#endif

  jReset();
  jAdd("{\"st\":"); jString(alarmStateName(alarmState));
  jAdd(",\"left\":%u,\"cause\":", (unsigned)stateSecondsLeft()); jString(alarmCause);
  jAdd(",\"warn\":%s,\"warnWhy\":", bs(earlyWarning)); jString(warnReason);
  jAdd(",\"alarms\":%lu,\"healthy\":%s", (unsigned long)alarmCount, bs(sensorsHealthy()));
  jAdd(",\"led\":[%u,%u,%u],\"buzz\":%u", ledPat[0], ledPat[1], ledPat[2], buzzPat);
  jAdd(",\"maint\":{\"on\":%s,\"nonce\":", bs(inMaintenance())); jString(maintNonce);
  jAdd(",\"usable\":%s,\"locked\":%u,\"fails\":%u,\"grants\":%lu,\"denials\":%lu}",
       bs(maintPinUsable()),
       (unsigned)(maintLockedUntil && (long)(millis() - maintLockedUntil) < 0
                  ? (maintLockedUntil - millis()) / 1000 : 0),
       maintFails, (unsigned long)maintGrants, (unsigned long)maintDenials);
  jAdd(",\"manual\":%s,\"manLed\":[%s,%s,%s],\"manBuzz\":%s,\"forced\":%d",
       bs(manualMode), bs(manLed[0]), bs(manLed[1]), bs(manLed[2]), bs(manBuzz), forcedState);
  jAdd(",\"muted\":%s,\"hasBuzzer\":%s", bs(buzzerMuted), bs(ENABLE_BUZZER));
  jAdd(",\"dhtOk\":%s,\"pirRaw\":%d,\"pirOk\":%s,\"pirWarm\":%u",
       bs(climateHealthy), pirRaw, bs(pirConfirmed),
       (unsigned)(millis() < PIR_WARMUP_MS ? (PIR_WARMUP_MS - millis()) / 1000 : 0));
  jAdd(",\"gas\":{\"raw\":%d,\"v\":%.2f,\"base\":%s,\"delta\":%s,\"rise\":%.2f,"
       "\"warn\":%s,\"crit\":%s,\"fast\":%s,\"ceil\":%.2f,\"warming\":%s}",
       lastGasRaw, isnan(lastGasVolts) ? 0.0f : lastGasVolts, gb, gdv,
       (double)gasRiseAmount, bs(gasWarn), bs(gasCrit), bs(gasRise),
       (double)gasCeiling(), bs(isnan(gasBaseline)));
#if ENABLE_IMU
  jAdd(",\"imu\":{\"ok\":%s,\"who\":%u,\"tilt\":%s,\"shock\":%s,\"spin\":%.0f,"
       "\"tamper\":%s,\"ref\":%s,\"warnDeg\":%.0f}",
       bs(imuOk), imuWho, tilt, shock, (double)imuSpin, bs(imuTamper),
       bs(imuHasRef()), (double)TILT_WARN_DEG);
#else
  jAdd(",\"imu\":null");
#endif
#if ENABLE_PRESENCE
  {
    const PresenceStats &p = presenceStats();
    const unsigned long since = millis() - p.lastSniffMs;
    jAdd(",\"pres\":{\"n\":%u,\"rnd\":%u,\"stable\":%u,\"rssi\":%d,\"amb\":%.1f,"
         "\"excess\":%.1f,\"streak\":%u,\"windows\":%u,\"learn\":%s,\"warn\":%s,"
         "\"on\":%s,\"next\":%lu,\"frames\":%lu}",
         p.count, p.randomized, p.stable, p.strongestRssi, (double)p.ambient,
         (double)p.excess, p.streak, p.windows, bs(p.learning), bs(p.warn),
         bs(sniffEnabled), since >= SNIFF_PERIOD_MS ? 0UL : (SNIFF_PERIOD_MS - since) / 1000,
         (unsigned long)p.frames);
  }
#else
  jAdd(",\"pres\":null");
#endif
  jAdd(",\"vision\":{\"fresh\":%s,\"known\":%s,\"unknown\":%s,\"name\":",
       bs(visionFresh()), bs(validatedPerson()), bs(visionFresh() && visionUnknown));
  jString(visionName);
  jAdd("}");
  jAdd(",\"test\":%s,\"heap\":%u,\"up\":%lu,\"rssi\":%d,\"drops\":%lu,\"build\":",
       bs(testActive), (unsigned)ESP.getFreeHeap(), millis() / 1000,
       (int)WiFi.RSSI(), (unsigned long)cmdDropped);
  jAdd("\"%s %s\"}", __DATE__, __TIME__);
}

#if ENABLE_MQTT
static void publishState() {
  if (!mqtt.connected()) return;
  buildStateJson();
  if (!jPublish(TOPIC_STATE, true)) return;   // retained
  lastStatePub = millis();
  stateDirty = false;
}
#endif

// The same JSON the dashboard gets, on the wire nobody can reach remotely.
static void publishStateLocal() {
  buildStateJson();
  Serial.printf("@@S %s\n", jbuf);
}

static void buildTestJson() {
  const unsigned long elapsed = !testActive ? 0
                              : testPaused  ? testPausedAt : millis() - testStepStart;
  const bool holding = testActive && !testPaused && testWaitConfirm
                    && TEST_STEPS[testStep].visual
                    && testResult[testStep] == RES_RUNNING && elapsed >= testStepMs;
  jReset();
  jAdd("{\"active\":%s,\"paused\":%s,\"loop\":%s,\"wait\":%s,\"done\":%s,\"hold\":%s,"
       "\"step\":%u,\"total\":%u,\"elapsed\":%lu,\"dur\":%u,\"passes\":%lu,\"res\":[",
       bs(testActive), bs(testPaused), bs(testLoop), bs(testWaitConfirm),
       bs(testFinished), bs(holding), testStep, TEST_COUNT, elapsed, testStepMs,
       (unsigned long)testPasses);
  for (uint8_t k = 0; k < TEST_COUNT; k++) jAdd(k ? ",%u" : "%u", testResult[k]);
  jAdd("],\"det\":[");
  for (uint8_t k = 0; k < TEST_COUNT; k++) { if (k) jAdd(","); jString(testDetail[k]); }
  jAdd("]}");
}

#if ENABLE_MQTT
static void publishTest() {
  if (!mqtt.connected()) return;
  buildTestJson();
  if (!jPublish(TOPIC_TEST, true)) return;
  testDirty = false;
}
#endif

// The step list never changes, so it is published once per connection and
// retained: the dashboard reads it whenever it opens.
static void buildTestMetaJson() {
  jReset();
  jAdd("{\"steps\":[");
  for (uint8_t k = 0; k < TEST_COUNT; k++) {
    if (k) jAdd(",");
    jAdd("{\"n\":"); jString(TEST_STEPS[k].name);
    jAdd(",\"h\":");  jString(TEST_STEPS[k].hint);
    jAdd(",\"v\":%u,\"ms\":%u}", TEST_STEPS[k].visual ? 1u : 0u, TEST_STEPS[k].ms);
  }
  jAdd("]}");
}

#if ENABLE_MQTT
static void publishTestMeta() {
  if (!mqtt.connected()) return;
  buildTestMetaJson();
  if (!jPublish(TOPIC_TESTMETA, true)) return;
  testMetaSent = true;
}
#endif

// =========================================================
// Commands
//
// A tiny JSON reader rather than ArduinoJson: the payloads are a handful of
// fields and the heap is already tight once BearSSL has taken its share.
// Everything is bounded - length, rate, and what each field may contain.
// =========================================================

// Copies the string value of "key" into out. Returns false when absent.
static bool jsonStr(const char *json, const char *key, char *out, size_t n) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  p++;
  while (*p == ' ') p++;
  if (*p != '"') return false;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i < n - 1) out[i++] = *p++;
  out[i] = '\0';
  return *p == '"';          // reject an unterminated string
}

static bool jsonNum(const char *json, const char *key, long *out) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  p++;
  while (*p == ' ') p++;
  if (*p != '-' && (*p < '0' || *p > '9')) return false;
  *out = strtol(p, nullptr, 10);
  return true;
}

static int ledIndex(const char *k) {
  if (!strcmp(k, "vert")  || !strcmp(k, "green"))  return 0;
  if (!strcmp(k, "jaune") || !strcmp(k, "yellow")) return 1;
  if (!strcmp(k, "rouge") || !strcmp(k, "red"))    return 2;
  return -1;
}

// Returns nullptr on success, or a reason. "trusted" is true for the serial
// console: USB access already means physical access, so it needs no PIN.
static const char *handleCommand(const char *json, bool trusted) {
  char s[72];
  long n = 0;

  // --- maintenance: the gate everything else sits behind ---
  if (jsonStr(json, "maint", s, sizeof(s))) {
    if (!strcmp(s, "off")) {
      if (!inMaintenance()) return "Pas en maintenance";
      leaveMaintenance("Maintenance quittee");
      return nullptr;
    }
    if (trusted && !strcmp(s, "on")) {
      enterState(AL_MAINT, MAINT_TIMEOUT_MS, "Maintenance (console serie)");
      return nullptr;
    }
    return maintTry(s);
  }

  // --- self-test ---
  if (jsonStr(json, "test", s, sizeof(s))) {
    long step = -1, flag = 0;
    jsonNum(json, "step", &step);
    jsonNum(json, "flag", &flag);
    if (trusted && !inMaintenance()) {
      enterState(AL_MAINT, MAINT_TIMEOUT_MS, "Maintenance (console serie)");
    }
    return testAction(s, step, flag);
  }

  // --- re-arm from anywhere ---
  if (jsonStr(json, "arm", s, sizeof(s))) {
    leaveMaintenance("Arme depuis le dashboard");
    return nullptr;
  }

  // Everything below changes what the box does, so it needs maintenance.
  // Refused by the BOARD, not merely hidden on the page.
  const bool allowed = inMaintenance() || trusted;
  if (!allowed) return "Passez en maintenance d'abord";

  if (jsonStr(json, "mode", s, sizeof(s))) {
    if (!strcmp(s, "manual")) {
      manualMode = true;
      manLed[0] = manLed[1] = manLed[2] = false;   // start dark: you see only what you set
      manBuzz = false;
      forcedState = -1;
    } else if (!strcmp(s, "auto")) {
      manualMode = false;
    } else return "mode : manual ou auto";
    return nullptr;
  }

  if (jsonStr(json, "force", s, sizeof(s))) {
    if      (!strcmp(s, "none"))  forcedState = -1;
    else if (!strcmp(s, "armed")) forcedState = AL_ARMED;
    else if (!strcmp(s, "entry")) forcedState = AL_ENTRY_DELAY;
    else if (!strcmp(s, "alarm")) forcedState = AL_ALARM;
    else return "force : none, armed, entry ou alarm";
    manualMode = false;
    return nullptr;
  }

  // Legacy {"led":"rouge"} - kept so the old dashboard buttons still work.
  if (jsonStr(json, "led", s, sizeof(s))) {
    ledOverride[0] = ledOverride[1] = ledOverride[2] = false;
    const int i = ledIndex(s);
    if (i >= 0) ledOverride[i] = true;
    else if (!strcmp(s, "blanc")) ledOverride[0] = ledOverride[1] = ledOverride[2] = true;
    else if (strcmp(s, "off") && strcmp(s, "eteint")) return "LED inconnue";
    ledOverrideUntil = millis() + LED_OVERRIDE_MS;
    return nullptr;
  }

  // Individual switches: {"vert":1}, {"rouge":0}...
  for (const char *k : {"vert", "jaune", "rouge", "green", "yellow", "red"}) {
    if (jsonNum(json, k, &n)) {
      manualMode = true;
      manLed[ledIndex(k)] = n != 0;
      return nullptr;
    }
  }

  if (jsonNum(json, "buzzer", &n)) {
    manualMode = true;
    manBuzz = n != 0;
    return nullptr;
  }

  if (jsonNum(json, "beep", &n)) {
    if (n < 20) n = 20;
    if (n > (long)BUZZER_BEEP_MAX_MS) n = BUZZER_BEEP_MAX_MS;
    buzzerOverrideState = true;
    buzzerOverrideUntil = millis() + (unsigned long)n;
    return nullptr;
  }

  if (jsonNum(json, "alloff", &n)) {
    manLed[0] = manLed[1] = manLed[2] = false;
    manBuzz = false;
    buzzerOverrideUntil = 0;
    return nullptr;
  }

  // Drill: run the real entry-delay -> alarm chain on demand. Leaves
  // maintenance deliberately, because that is the chain being tested.
  if (jsonStr(json, "drill", s, sizeof(s))) {
    if (!strcmp(s, "entry")) {
      manualMode = false;
      forcedState = -1;
      enterState(AL_ENTRY_DELAY, ENTRY_DELAY_MS, "EXERCICE : detection simulee");
      publishAlert("exercice", "attention", "Exercice : temporisation d'entree");
      return nullptr;
    }
    if (!strcmp(s, "alarm")) {
      manualMode = false;
      forcedState = -1;
      alarmCount++;
      enterState(AL_ALARM, ALARM_DURATION_MS, "EXERCICE : alarme simulee");
      publishAlert("exercice", "critique", "Exercice : alarme");
      return nullptr;
    }
    return "drill : entry ou alarm";
  }

  if (jsonNum(json, "mute", &n))  { buzzerMuted = n != 0; return nullptr; }
  if (jsonNum(json, "sniff", &n)) { sniffEnabled = n != 0; return nullptr; }

  // Injecte ce que la camera verrait, pour pouvoir eprouver les deux regles
  // depuis la console serie, sans reseau ni visage reel.
  //   {"face":"known","name":"Kosta"}   {"face":"unknown"}   {"face":"none"}
  if (jsonStr(json, "face", s, sizeof(s))) {
    visionKnown   = strcmp(s, "known") == 0;
    visionUnknown = strcmp(s, "unknown") == 0;
    visionSeenMs  = millis();
    if (!jsonStr(json, "name", visionName, sizeof(visionName))) visionName[0] = '\0';
    Serial.printf("[vision] (injecte) %s%s%s\n", s,
                  visionName[0] ? " : " : "", visionName);
    return nullptr;
  }

  if (jsonStr(json, "rebase", s, sizeof(s))) {
#if ENABLE_IMU
    if (!strcmp(s, "imu")) { imuRelearn(); return nullptr; }
#endif
#if ENABLE_GAS
    if (!strcmp(s, "gas")) { gasRelearn(); return nullptr; }
#endif
#if ENABLE_PRESENCE
    if (!strcmp(s, "presence")) { presenceRelearn(); return nullptr; }
#endif
    return "rebase : imu, gas ou presence";
  }

  return "Commande inconnue";
}

#if ENABLE_MQTT
static void mqttCallback(char *topic, byte *payload, unsigned int length) {
  if (length == 0 || length > CMD_MAX_BYTES) { cmdDropped++; return; }

  // --- ce que la camera voit ---
  if (strcmp(topic, TOPIC_VISION) == 0) {
    char msg[CMD_MAX_BYTES + 1];
    memcpy(msg, payload, length);
    msg[length] = '\0';
    char face[16] = {0};
    if (!jsonStr(msg, "face", face, sizeof(face))) return;
    const bool wasKnown = visionKnown, wasUnknown = visionUnknown;
    visionKnown   = strcmp(face, "known") == 0;
    visionUnknown = strcmp(face, "unknown") == 0;
    visionSeenMs  = millis();
    if (!jsonStr(msg, "name", visionName, sizeof(visionName))) visionName[0] = '\0';
    if (visionKnown != wasKnown || visionUnknown != wasUnknown) {
      Serial.printf("[vision] %s%s%s\n", face,
                    visionName[0] ? " : " : "", visionName);
      stateDirty = true;
    }
    return;
  }

  if (strcmp(topic, TOPIC_CMD) != 0) return;

  // Oversized payloads are dropped unread rather than truncated into
  // something that happens to parse.
  if (length == 0 || length > CMD_MAX_BYTES) {
    cmdDropped++;
    Serial.printf("[cmd] rejete : %u octets\n", length);
    return;
  }

  const unsigned long now = millis();
  if (now - cmdWindowStart >= 1000) { cmdWindowStart = now; cmdThisSecond = 0; }
  if (++cmdThisSecond > CMD_RATE_PER_S) {
    cmdDropped++;
    return;                       // silently: answering a flood feeds it
  }

  char msg[CMD_MAX_BYTES + 1];
  memcpy(msg, payload, length);
  msg[length] = '\0';

  const char *err = handleCommand(msg, false);
  stateDirty = true;

  // The reply always carries the NEW nonce, in the retained state, so the
  // dashboard can immediately try again after a wrong PIN.
  if (err) {
    Serial.printf("[cmd] refuse : %s\n", err);
    jReset();
    jAdd("{\"type\":\"cmd\",\"niveau\":\"info\",\"msg\":"); jString(err);
    jAdd(",\"source\":\"esp8266\"}");
    jPublish(TOPIC_ALERTS, false);
  }
  publishState();
}
#endif

// =========================================================
// Network
// =========================================================

#if ENABLE_PRESENCE && PRESENCE_SNIFFER
// A sniff window tears the radio down; this puts it back.
static void rebuildNetwork() {
  #if WIFI_JOIN_STATION
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  #endif
}
#endif

#if ENABLE_MQTT
// Bounded and backed off: a missing broker costs a connect attempt every so
// often, never a stalled loop. The sensors and siren do not need the network.
static void serviceMqtt() {
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (WiFi.status() != WL_CONNECTED) return;

  const unsigned long now = millis();
  if (mqttRetryAt && (long)(now - mqttRetryAt) < 0) return;

  char clientId[40];
  snprintf(clientId, sizeof(clientId), "sentinelx-%s-esp8266", GROUP_ID);

  // Last will: if the box disappears, the broker says so on our behalf.
  if (mqtt.connect(clientId, MQTT_USERNAME, MQTT_PASSWORD,
                   TOPIC_STATUS, 1, true, "offline")) {
    Serial.println(F("[mqtt] connecte"));
    mqtt.subscribe(TOPIC_CMD, 1);
    mqtt.subscribe(TOPIC_VISION, 0);     // flux continu : QoS 0 suffit
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqttBackoff = MQTT_RETRY_MIN_MS;
    mqttRetryAt = 0;
    newNonce();                 // fresh challenge for every new session
    testMetaSent = false;
    publishTestMeta();
    publishState();
    publishTest();
    return;
  }

  Serial.printf("[mqtt] echec, etat=%d, nouvel essai dans %lu s\n",
                mqtt.state(), mqttBackoff / 1000);
  #if MQTT_USE_TLS
  // etat=-2 covers every network and TLS problem, which is useless on its own.
  // BearSSL knows exactly what went wrong, so print it: it is the difference
  // between "wrong CA pasted" and "certificate not yet valid".
  {
    char err[80] = {0};
    netClient.getLastSSLError(err, sizeof(err));
    if (err[0]) Serial.printf("[mqtt] tls: %s\n", err);
    Serial.printf("[mqtt] horloge carte: %lu (doit etre dans la validite du certificat)\n",
                  (unsigned long)BUILD_EPOCH);
  }
  #endif
  mqttRetryAt = now + mqttBackoff;
  mqttBackoff = mqttBackoff * 2 > MQTT_RETRY_MAX_MS ? MQTT_RETRY_MAX_MS : mqttBackoff * 2;
}
#endif

// =========================================================
// Serial console - trusted, because USB means physical access.
//
//   s                     state as JSON          -> @@S {...}
//   r                     sensor readings        -> @@R {...}
//   t / m                 self-test state / steps
//   {"..."}               any command, verbatim
//   maint on / maint off  shortcut, no PIN needed here
// =========================================================

static void serialCommand(char *line) {
  if (!strcmp(line, "s"))      { publishStateLocal(); }
  else if (!strcmp(line, "r")) { jReset(); buildTelemetryJson(); Serial.printf("@@R %s\n", jbuf); }
  else if (!strcmp(line, "t")) { buildTestJson(); Serial.printf("@@T %s\n", jbuf); }
  else if (!strcmp(line, "m")) { buildTestMetaJson(); Serial.printf("@@M %s\n", jbuf); }
  else if (!strncmp(line, "maint ", 6)) {
    char j[CMD_MAX_BYTES + 16];
    snprintf(j, sizeof(j), "{\"maint\":\"%s\"}", line + 6);
    const char *e = handleCommand(j, true);
    Serial.printf(e ? "@@ERR %s\n" : "@@OK\n", e);
    stateDirty = true;
  }
  else if (line[0] == '{') {
    const char *e = handleCommand(line, true);
    Serial.printf(e ? "@@ERR %s\n" : "@@OK\n", e);
    stateDirty = true;
  }
  else if (line[0]) Serial.printf("@@ERR commande inconnue : %s\n", line);
}

static void serialConsole() {
  static char line[CMD_MAX_BYTES + 1];
  static uint16_t len = 0;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { line[len] = '\0'; serialCommand(line); len = 0; }
    else if (len < sizeof(line) - 1) line[len++] = c;
  }
}

// =========================================================
// Setup
// =========================================================

void setup() {
#if ENABLE_BUZZER
  // First statement in the program: GPIO2's strap pull-up holds the pin high
  // from reset, which sounds an active-high buzzer, so the sooner this runs the
  // shorter the chirp at boot.
  pinMode(PIN_BUZZER, OUTPUT);
  buzzerWrite(false);
#endif

  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println();
  Serial.println(F("================ Sentinel-X ================"));
  Serial.printf("carte   : ESP8266 (ESP-12F), chip %08X\n", ESP.getChipId());
  Serial.printf("build   : %s %s\n", __DATE__, __TIME__);

#if ENABLE_LEDS
  for (uint8_t i = 0; i < 3; i++) { pinMode(LED_PINS[i], OUTPUT); ledWrite(LED_PINS[i], false); }
  // Brief sweep: an LED that never lights here is wired wrong, not idle.
  for (uint8_t i = 0; i < 3; i++) { ledWrite(LED_PINS[i], true); delay(180); ledWrite(LED_PINS[i], false); }
#endif
#if ENABLE_CLIMATE
  dht.begin();
#endif
#if ENABLE_MOTION
  pinMode(PIN_PIR, INPUT);
  pirRaw = digitalRead(PIN_PIR);
  pirSince = millis();
#endif
#if ENABLE_IMU
  imuWho = imuBegin();
  imuOk = imuWho != 0;
  Serial.printf("imu     : %s\n", imuOk ? "MPU-6500 present"
                                        : "pas de reponse au demarrage, nouvel essai en continu");
#endif
#if ENABLE_PRESENCE
  presenceReset();
#endif

  newNonce();
  if (!maintPinUsable()) {
    Serial.println(F("maint   : PIN non configure -> maintenance a distance REFUSEE."));
    Serial.println(F("          Definir MAINT_PIN dans include/secrets.h."));
  }

#if WIFI_JOIN_STATION
  // Non-blocking: begin() and carry on. A missing hotspot must never stall the
  // sensors or the siren.
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.hostname("sentinel-x");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("wifi    : connexion a %s en arriere-plan\n", WIFI_SSID);
#endif

#if ENABLE_MQTT
  #if MQTT_USE_TLS
  netClient.setTrustAnchors(&caCertList);
  // No RTC, and an isolated hotspot has no NTP, so the clock is seeded from
  // the build date purely so certificate dates can be checked at all.
  netClient.setX509Time(BUILD_EPOCH);
  netClient.setBufferSizes(MQTT_TLS_RX_BUFFER, MQTT_TLS_TX_BUFFER);
  #endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
  mqtt.setSocketTimeout((uint16_t)(MQTT_CONNECT_TIMEOUT_MS / 1000));
#endif

  // Starts in exit delay, so the box can be put down and left before it watches.
  enterState(AL_EXIT_DELAY, EXIT_DELAY_MS, "Demarrage");
  Serial.printf("heap    : %u octets libres\n", ESP.getFreeHeap());
  Serial.println(F("============================================"));
}

// =========================================================
// Loop
// =========================================================

void loop() {
  const unsigned long now = millis();
  static unsigned long lastClimate = 0, lastGas = 0;
#if ENABLE_IMU
  static unsigned long lastImu = 0;
#endif
  static unsigned long lastOut = 0, lastLog = 0;
  static AlarmState prevState = AL_EXIT_DELAY;

#if ENABLE_MOTION
  pollMotion();          // every pass: a brief trip must not be missed
#endif
#if ENABLE_CLIMATE
  if (now - lastClimate >= CLIMATE_INTERVAL_MS) { lastClimate = now; readClimate(); }
#endif
#if ENABLE_GAS
  if (now - lastGas >= GAS_INTERVAL_MS) { lastGas = now; readGas(); }
#endif
#if ENABLE_IMU
  if (now - lastImu >= IMU_INTERVAL_MS) { lastImu = now; readImu(); }
#endif

#if ENABLE_PRESENCE && PRESENCE_SNIFFER
  // Scheduled last among the sensors: a window blocks and drops the link, so
  // everything else is serviced first. Held while someone is driving the box
  // from the dashboard, where losing the link mid-action would be worse than
  // missing one window.
  if (presenceSniffDue()) {
    if (sniffEnabled && !inMaintenance() && !testActive) {
      presenceRunSniffWindow(rebuildNetwork);
      const PresenceStats &p = presenceStats();
      Serial.printf("[sniff] %u appareils (%u aleatoires), ambiant %.1f, ecart %+.1f%s\n",
                    p.count, p.randomized, (double)p.ambient, (double)p.excess,
                    p.learning ? " [apprentissage]" : "");
      stateDirty = true;
    } else {
      presenceDeferSniff();
    }
  }
#endif

  if (testActive) testTick();
  evaluateAlarm();

  if (now - lastOut >= LED_REFRESH_MS) { lastOut = now; applyOutputs(); }

  // A state change is worth telling the dashboard about immediately.
  if (alarmState != prevState) {
    prevState = alarmState;
    stateDirty = true;
    if (alarmState == AL_ALARM) publishAlert("intrusion", "critique", alarmCause);
    else if (alarmState == AL_ENTRY_DELAY) publishAlert("intrusion", "attention", alarmCause);
  }

#if ENABLE_MQTT
  serviceMqtt();
  if (mqtt.connected()) {
    // Faster heartbeat during a countdown so the dashboard's timer tracks the
    // board's rather than drifting away from it.
    const unsigned long period =
      (alarmState == AL_ENTRY_DELAY || alarmState == AL_EXIT_DELAY || testActive)
        ? STATE_FAST_MS : STATE_HEARTBEAT_MS;
    if (stateDirty || now - lastStatePub >= period) publishState();
    if (testDirty) publishTest();
    if (!testMetaSent) publishTestMeta();
    if (now - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
      lastTelemetry = now;
      publishTelemetry();
    }
  }
#endif

  serialConsole();

  if (now - lastLog >= SERIAL_LOG_INTERVAL_MS) {
    lastLog = now;
    Serial.printf("[%6lus] %-11s", now / 1000, alarmStateName(alarmState));
    if (stateSecondsLeft()) Serial.printf(" (%u s)", stateSecondsLeft());
    Serial.printf(" | %.1fC %.0f%%HR | pir=%d | gaz %.2fV",
                  lastCelsius, lastHumidity, pirConfirmed ? 1 : 0,
                  isnan(lastGasVolts) ? 0.0f : lastGasVolts);
#if ENABLE_IMU
    if (imuHasRef()) Serial.printf(" | inclin %.1f deg%s", imuTilt, imuTamper ? " SABOTAGE" : "");
#endif
#if ENABLE_PRESENCE
    Serial.printf(" | wifi %u/%.1f", presenceStats().count, (double)presenceStats().ambient);
#endif
    Serial.printf(" | heap %u\n", ESP.getFreeHeap());
  }

  delay(2);   // feeds the SDK's housekeeping
}
