// =========================================================
// Sentinel-X — ALL tunable settings live in this one file.
//
// Target: NodeMCU LoLin V3 / ESP8266MOD (ESP-12F)
// PlatformIO env: nodemcuv2
//
// Secrets (WiFi password, MQTT password, maintenance PIN, CA cert) are NOT
// here. They live in include/secrets.h, which git ignores. See
// secrets.example.h.
//
// The board has no screen and no web page of its own. Everything is watched
// and driven from the main dashboard (../dashboard, served by nginx on the
// server PC), which talks to the board over MQTT only.
// =========================================================
#pragma once

#include "secrets.h"

// =========================================================
// 1. FEATURE FLAGS — turn whole subsystems on/off
//
// Each is wrapped in #ifndef so a PlatformIO env can override it with
// -DENABLE_x=0 without editing this file. See platformio.ini.
// =========================================================

#ifndef ENABLE_CLIMATE
  #define ENABLE_CLIMATE        1   // DHT22 temperature + humidity
#endif
#ifndef ENABLE_GAS
  #define ENABLE_GAS            1   // MQ-x analog gas sensor on A0
#endif
#ifndef ENABLE_MOTION
  #define ENABLE_MOTION         1   // HC-SR501 PIR
#endif
#ifndef ENABLE_IMU
  #define ENABLE_IMU            1   // MPU-6500: tilt / shock / rotation tamper
#endif
#ifndef ENABLE_LEDS
  #define ENABLE_LEDS           1   // the 3 status LEDs
#endif
#ifndef ENABLE_BUZZER
  #define ENABLE_BUZZER         1   // READ the PIN_BUZZER wiring notes first
#endif
#ifndef ENABLE_PRESENCE
  #define ENABLE_PRESENCE       1   // WiFi device detection (section 5)
#endif
#ifndef ENABLE_MQTT
  #define ENABLE_MQTT           1   // the only way in and out (section 8)
#endif

// =========================================================
// 2. PIN MAP  (ESP8266 — do not copy ESP32 GPIO numbers here)
//
// Free pins on this board are scarce. D1/D2/D6/D7/A0 are taken
// by sensors, TX/RX are the serial console, so the LEDs get
// D5/D0/D8 and the buzzer gets D3.
// =========================================================

#define PIN_DHT                 D2      // GPIO4  — DHT22 DATA
#define PIN_PIR                 D1      // GPIO5  — HC-SR501 OUT (3V3 logic, safe direct)
#define PIN_GAS                 A0      // the only ADC; MQ AO via 68k+68k divider
#define PIN_IMU_SDA             D6      // GPIO12 — MPU-6500 SDA
#define PIN_IMU_SCL             D7      // GPIO13 — MPU-6500 SCL

// The 3 status LEDs. All three are ACTIVE-HIGH: pin -> 220R -> LED -> GND.
#define PIN_LED_GREEN           D5      // GPIO14 — validation / all clear
#define PIN_LED_YELLOW          D0      // GPIO16 — early warning
#define PIN_LED_RED             D8      // GPIO15 — intruder

// !! GPIO15 (D8) MUST be active-high. It has an onboard pulldown and the chip
// !! refuses to boot if it is high at reset. Wiring the red LED the other way
// !! round (3V3 -> LED -> D8) bricks boot until you unplug it.

// Buzzer: ACTIVE type (built-in oscillator, so a plain level drives it), on
// D4/GPIO2.
//
// ===== AS BUILT: direct drive, ACTIVE-HIGH =====
//
//      D4 ---[100R]---[buzzer]--- GND        BUZZER_ACTIVE_LOW 0
//
// Driving GPIO2 HIGH sounds it. The 100R limits the current the pin has to
// source; measure the voltage across it while sounding to get the real figure,
// I(mA) = V * 10.
//
// Two consequences of this wiring, neither of them software bugs:
//
//   * GPIO2 is a boot strap pin that wants to be HIGH at reset, and a load to
//     GND pulls against that. It works here because an active buzzer's DC
//     impedance is high enough, but it is working despite the wiring rather
//     than because of it. An intermittent failure to boot, or a swapped
//     buzzer, should send you straight back here.
//   * The strap pull-up holds GPIO2 high before setup() runs, so the buzzer
//     CHIRPS BRIEFLY AT EVERY BOOT. setup() silences it as its first action to
//     keep that as short as possible. Expected, not a fault.
//
// ===== ALTERNATIVE: PNP high-side, ACTIVE-LOW =====
//
// The cleaner build, and what BUZZER_ACTIVE_LOW 1 expects. It keeps the strap
// behaviour correct (the buzzer then acts as a pull-up on GPIO2) and takes the
// buzzer current off the pin entirely.
//
// !! If you go back to that, GPIO2 MUST be HIGH at reset, which forces two
// !! things, and getting either wrong stops the board booting:
// !!
// !!   1. The buzzer is ACTIVE-LOW. Driven LOW = sounding.
// !!   2. The switch must be PNP (high-side), not NPN (low-side). An NPN is
// !!      active-high, and its 1k base resistor would clamp GPIO2 near 0.7 V
// !!      at reset, which reads as LOW and the chip never starts.
// !!
// !!             3V3
// !!              |
// !!         E ---+---  PNP  (2N3906 / BC557 / S8550)
// !!   GPIO2 -[1k]- B
// !!         C ---+---
// !!              |
// !!          [buzzer]
// !!              |
// !!             GND
// !!
// !! The 1k sets base current to ~2.6 mA, enough to saturate the transistor
// !! for a 25-30 mA buzzer while the GPIO itself carries almost nothing.
// !! Do NOT use ~100R there: the pin would sink ~26 mA, over its rating, which
// !! defeats the point of the transistor.
// !!
// !! Keep the buzzer on 3V3, NOT 5 V. A 5 V emitter sits 1.7 V above a 3.3 V
// !! GPIO, so Vbe never reaches 0 and the buzzer could never be switched off.
// !!
// !! GPIO2 also carries the module's onboard blue LED (also active-low), so it
// !! flashes along with the buzzer. Handy: with no buzzer wired yet, that LED
// !! previews the buzzer pattern for you.
#define PIN_BUZZER              D4      // GPIO2 — see the two wiring options above

// 0 = D4 -> 100R -> buzzer -> GND   (as built; HIGH sounds it)
// 1 = 3V3 -> buzzer -> 100R -> D4   (PNP or direct; LOW sounds it)
#define BUZZER_ACTIVE_LOW       0

// =========================================================
// 3. TIMING  (all milliseconds)
// =========================================================

#define CLIMATE_INTERVAL_MS     2000UL      // DHT22 refuses reads closer than 2 s
#define GAS_INTERVAL_MS         250UL       // 4 Hz: a lighter's gas is caught in <1 s
#define IMU_INTERVAL_MS         50UL        // 20 Hz: a knock lasts tens of ms
#define TELEMETRY_INTERVAL_MS   5000UL      // sensor readings for the API and the AI
#define LED_REFRESH_MS          50UL        // drives the blink patterns

// Warm-up windows. Readings before these are published but flagged.
#define PIR_WARMUP_MS           60000UL     // HC-SR501 false-trips for ~1 min
#define GAS_WARMUP_MS           180000UL    // MQ heater needs ~3 min before a baseline
                                            // means anything (new sensor: 24-48 h)

// =========================================================
// 4. SENSOR CALIBRATION
// =========================================================

#define DHT_TYPE                DHT22       // blue housing = DHT11, white = DHT22

// ESP8266 ADC is 10-bit: 0..1023. NOT 0..4095 like the ESP32.
#define ADC_MAX                 1023.0f
#define ADC_FULL_SCALE_V        3.3f        // NodeMCU's own 220k/100k divider sets this

// Two equal resistors on the MQ's AO, so A0 sees half of what the sensor outputs.
// Set to 1.0 if you ever wire AO straight in (don't — AO swings to 5 V).
#define GAS_DIVIDER_RATIO       2.0f

// The gas volts shown are only as true as GAS_DIVIDER_RATIO. Two resistors that
// are not exactly equal, plus the NodeMCU's own 220k/100k divider loading A0,
// shift the real ratio. On the bench a 5 V sensor read 6 V on screen with gas
// applied - physically impossible, so the displayed scale is off.
//
// That does not matter for detection, which works on CHANGES and scales with
// them. To make the absolute volts true: measure the sensor's AO pin with a
// multimeter, note the gas volts the dashboard shows at the same moment, and
// set GAS_DIVIDER_RATIO = 2.0 * measured / shown.

// --- gas: two independent detectors -----------------------------------------
//
// 1. RAPID RISE - the fast one. Compares the reading with the lowest value of
//    the last GAS_RISE_WINDOW_MS. A lighter's gas on the bench took the reading
//    from 5 V to 6+ V in a couple of seconds; this trips on that in well under
//    a second, and needs no baseline, so it works from GAS_RISE_ARM_MS after
//    boot instead of waiting three minutes for the heater.
//
// 2. DELTA ABOVE BASELINE - the slow one, for a leak that creeps up too
//    gently to look like a rise. The baseline follows slow drift (heater,
//    temperature) but freezes as soon as the reading moves away from it, so a
//    real leak cannot be "learned" as normal.
#define GAS_RISE_V              0.40f       // rise within the window -> critical
#define GAS_RISE_WINDOW_MS      5000UL
#define GAS_RISE_ARM_MS         30000UL     // heater settling right after power-up
#define GAS_BASELINE_SAMPLES    20          // averaged after warm-up (5 s at 4 Hz)
#define GAS_BASELINE_FOLLOW     0.0005f     // drift tracking per sample (~8 min)
#define GAS_WARN_DELTA_V        0.35f       // above baseline -> early warning
#define GAS_CRIT_DELTA_V        0.90f       // above baseline -> critical
#define GAS_HOLD_MS             10000UL     // a gas state holds this long after clearing

// --- tamper: the box being tilted, knocked or turned ------------------------
//
// Three independent signals, any of which counts as "the box was touched":
//
//   TILT      angle between the current gravity vector and the one captured
//             when the system armed. Vector angle, not pitch/roll: near
//             pitch +/-90 roll is atan2 of two near-zero numbers and swung
//             17 deg on the bench with the board perfectly still.
//   SHOCK     total acceleration departing from its resting value.
//   ROTATION  the gyro, against a bias measured at rest.
//
// Bench noise with the board still: tilt < 1 deg, |a| +/- 0.02 g, gyro well
// under 1 deg/s - so these thresholds sit far above the noise floor.
#define TILT_WARN_DEG           8.0f        // was 25: far too lax to notice a nudge
#define IMU_SHOCK_DELTA_G       0.25f       // departure from the resting |a|
#define IMU_GYRO_DPS            15.0f       // rotation on any axis, bias removed
#define IMU_REF_SAMPLES         20          // reads averaged into the reference (1 s)
// Le MPU est re-sonde tant qu'il ne repond pas, au lieu d'etre declare absent
// une fois pour toutes au demarrage. Une seule sonde ratee au boot condamnait
// sinon la detection de sabotage pour toute la session - constate au banc :
// le scanner I2C trouvait 0x68 sans probleme pendant que le firmware affichait
// "ABSENT". Un module rebranche en cours de route est aussi recupere.
#define IMU_RETRY_MS            3000UL
#define IMU_BASELINE_MS         5000UL      // settle this long before capturing it
#define IMU_TAMPER_CLEAR_MS     3000UL      // must be still this long to clear

// --- PIR --------------------------------------------------------------------
//
// The HC-SR501 holds OUT high for as long as its TIME knob says - up to ~5 min
// - and retriggers while anything moves. No firmware can shorten that. If
// motion "never turns off", turn the module's time knob to minimum (usually
// fully anticlockwise, ~3 s), and the sensitivity knob down if it trips from
// across the room.
//
// The firmware's part: a level must hold PIR_CONFIRM_MS to count, and only
// the RISING edge of confirmed motion starts an alarm, so a sensor stuck high
// raises one alarm rather than an endless one.
#define PIR_CONFIRM_MS          1000UL

// =========================================================
// 5. PRESENCE DETECTION  (WiFi probe sniffer)
//
// Promiscuous-mode capture of 802.11 probe requests from phones that are not
// connected to anything of ours. This is the "through walls" signal.
//
// Why the first version was far too sensitive, and what changed:
//
//   * It counted every MAC seen in the last 5 minutes, down to -80 dBm. In a
//     building that is always several devices - neighbours, passers-by, your
//     own phone - so it warned at once and never cleared. Now each window
//     counts only what it heard ITSELF, and only nearby (-65 dBm).
//   * It compared that count with a fixed number. Now it LEARNS the usual
//     level of the place for PRESENCE_LEARN_WINDOWS windows, and warns only on
//     a clear excess over it, sustained for PRESENCE_CONFIRM_WINDOWS windows
//     in a row.
//   * Presence alone is only ever an EARLY WARNING (yellow). WiFi activity is
//     too indirect to call someone an intruder; that takes the PIR or tamper.
//
// Read these before tuning:
//   * Each sniff window drops the WiFi link, so MQTT reconnects every period.
//   * Phones RANDOMIZE their MAC in probe requests, so counts are an activity
//     level, not a headcount.
//   * This captures identifiers from the air. Fine for your own workshop; know
//     what it is before pointing it at a public space.
// =========================================================

#ifndef PRESENCE_SNIFFER
  #define PRESENCE_SNIFFER      1
#endif

// Le sniffer est-il ACTIF AU DEMARRAGE ?
//
// 0 par defaut, et ce n'est pas un recul : chaque fenetre coupe le WiFi, donc
// le lien MQTT tombe et se reconnecte. Mesure au banc : le boitier disparait du
// dashboard ~8 s toutes les 60 s, soit plus de 10 % du temps. Pour une demo
// c'est insupportable, et la detection de presence n'est qu'une pre-alerte.
//
// Il s'allume d'un bouton depuis le dashboard quand on veut le montrer, et
// SNIFF_PERIOD_MS reste reglable si on le veut en permanence mais plus espace.
#define SNIFF_ON_AT_BOOT        0

#define SNIFF_PERIOD_MS         60000UL     // how often a sniff window opens
#define SNIFF_WINDOW_MS         3000UL      // how long the link is down for it
#define SNIFF_CHANNELS          {1, 6, 11}  // 2.4 GHz non-overlapping channels
#define SNIFF_DWELL_MS          300UL       // time per channel before hopping
#define PRESENCE_TABLE_SIZE     40          // distinct MACs per window (10 B each)

#define SNIFF_MIN_RSSI          -65         // dBm. Was -80: that is next door.
#define PRESENCE_LEARN_WINDOWS  5           // windows learning the usual level
#define PRESENCE_WARN_EXCESS    3.0f        // devices above the usual level...
#define PRESENCE_CONFIRM_WINDOWS 2          // ...for this many windows in a row
#define PRESENCE_AMBIENT_FOLLOW 0.1f        // usual level drifts with quiet windows

// =========================================================
// 6. ALARM - how a detection becomes an alarm
//
// Modelled on a real alarm panel:
//
//   ARMED, QUIET    green. Watching.
//   ENTRY DELAY     yellow, fast blink, a countdown beep every second. Motion
//                   or tamper was detected: whoever it is has ENTRY_DELAY_MS
//                   to identify themselves by switching the box to
//                   maintenance from the dashboard (PIN required).
//   ALARM           red, alarm burst. Nobody identified in time. Lasts
//                   ALARM_DURATION_MS, then re-arms. Only a NEW detection
//                   starts a new cycle - a sensor stuck high does not keep the
//                   siren going forever.
//   MAINTENANCE     green/yellow alternating, silent. Detections are still
//                   measured and shown but never alarm.
//
// Alongside that:
//   EARLY WARNING   yellow, slow blink: unusual WiFi presence or a gas warning.
//                   Silent by default (BUZZER_EARLY_WARNING).
//   GAS CRITICAL    red immediately. Gas is a safety hazard, not an intrusion:
//                   no entry delay, and it still sounds in maintenance unless
//                   MAINT_SILENCES_GAS is set.
// =========================================================

#define ENTRY_DELAY_MS          10000UL     // temps pour s'identifier avant l'alarme

// A la fin de la temporisation d'entree, faut-il que quelque chose soit ENCORE
// detecte pour declencher ?
//
// 1 (defaut) : oui. Quelqu'un qui passe devant le PIR et s'en va ne declenche
//              rien. Sans ca, le moindre passage dans le couloir finit en
//              sirene 30 s plus tard alors que la piece est vide depuis
//              longtemps - c'est le defaut classique des alarmes mal reglees,
//              et la raison pour laquelle on finit par les debrancher.
// 0          : comportement strict, toute detection finit en alarme.
//
// Le SABOTAGE est toujours une exception : un boitier deplace reste une alarme
// meme s'il ne bouge plus, parce que le deplacement, lui, a bien eu lieu.
#define ALARM_NEEDS_LIVE_TRIGGER 1

// --- ce que la camera voit ------------------------------------------------
//
// Le service vision publie sur le topic "vision" : visage connu, inconnu, ou
// rien. Le boitier le traite comme un capteur de plus, et deux regles en
// decoulent :
//
//   PERSONNE AUTORISEE EN VUE  -> aucune alarme d'intrusion. C'est l'etat
//      "valide" : quelqu'un de reconnu est la, le mouvement et le deplacement
//      du boitier sont normaux. Le GAZ reste une exception : c'est un risque
//      pour les personnes, pas une intrusion, et personne ne doit pouvoir le
//      faire taire en etant reconnu.
//
//   VISAGE INCONNU EN VUE      -> VISION_IDENTIFY_MS pour se faire reconnaitre
//      en se presentant face a la camera. Passe ce delai, alarme. Si une
//      personne autorisee apparait entre-temps, le decompte est annule.
//
// Delai court exprès : la personne est deja devant l'objectif, il ne s'agit
// que de montrer son visage. La temporisation longue d'ENTRY_DELAY_MS sert
// a quelqu'un qui entre et doit traverser la piece.
#define VISION_IDENTIFY_MS      10000UL

// Sans nouvelle de la camera pendant ce temps, on oublie ce qu'elle disait.
// Sinon un service vision arrete laisserait le boitier croire eternellement
// qu'une personne autorisee est presente - une porte ouverte permanente.
#define VISION_STALE_MS         20000UL

// Fenetre pendant laquelle une detection compte encore comme "en cours" a
// l'expiration. Le PIR retombe entre deux passages : exiger qu'il soit haut
// a la milliseconde pres raterait quelqu'un qui est bel et bien la.
#define TRIGGER_RECENT_MS       3000UL
#define EXIT_DELAY_MS           5000UL      // apres armement : temps pour sortir
#define ALARM_DURATION_MS       60000UL     // siren length, then re-arm

// An old-style MQTT {"led":"..."} command seizes the LEDs for this long (only
// in maintenance), then hands them back. Bounded so a forgotten command cannot
// leave the box lying about what it sees.
#define LED_OVERRIDE_MS         10000UL

// --- buzzer patterns --------------------------------------------------------
//
// An active buzzer has one pitch and one loudness, so RHYTHM is the only way
// to tell the states apart by ear. All non-blocking.
//
// ALARM: a burst of beeps then a pause. Not a plain duty cycle: at ~3 Hz an
// active buzzer's tone plus the chopping sounds like one continuous drone
// (measured on the bench). Keep BUZZER_RED_BEEP_MS well above ~100 ms.
#define BUZZER_RED_BEEPS        3
#define BUZZER_RED_BEEP_MS      120UL
#define BUZZER_RED_GAP_MS       120UL
#define BUZZER_RED_PAUSE_MS     700UL

// ENTRY DELAY: one short beep a second, like a real panel's countdown.
#define BUZZER_ENTRY_ON_MS      80UL
#define BUZZER_ENTRY_PERIOD_MS  1000UL

// EARLY WARNING: one chirp every few seconds - OFF by default. WiFi presence
// is common enough that chirping for it is exactly the noise people complain
// about; the yellow LED and the dashboard carry it instead.
#define BUZZER_EARLY_WARNING    0
#define BUZZER_YELLOW_ON_MS     100UL
#define BUZZER_YELLOW_PERIOD_MS 4000UL

// A {"beep":ms} command, maintenance only, capped at this.
#define BUZZER_BEEP_MAX_MS      3000UL

// =========================================================
// 7. MAINTENANCE MODE - and how it is protected
//
// Maintenance is the ONE unlocked state. Inside it: the buzzer is quiet, nothing
// alarms, and the dashboard may switch to manual, preview states, run the
// self-test, recalibrate and turn the sniffer off. Outside it, all of that is
// refused by the board itself - not just greyed out on the page.
//
// Getting in needs the PIN (MAINT_PIN, in secrets.h), proven by challenge-
// response so the PIN never crosses the network:
//
//   1. The board publishes a fresh random nonce in its state.
//   2. The dashboard sends sha256(nonce + ":" + PIN).
//   3. The board compares in constant time. The nonce is single-use: it is
//      replaced after every attempt, right or wrong, so a captured answer is
//      worthless.
//
// Only the board may read the cmd topic (Mosquitto ACL), so nobody else can
// even collect answers to brute-force offline.
//
// After MAINT_MAX_FAILS wrong answers it locks for MAINT_LOCKOUT_MS, and every
// attempt is published as a security event. Maintenance ends by itself after
// MAINT_TIMEOUT_MS so the box is never left disarmed by accident; on the way
// out every override is cleared and the sniffer comes back on.
// =========================================================

#define MAINT_TIMEOUT_MS        1800000UL   // 30 min, then re-arms on its own
#define MAINT_MAX_FAILS         5
#define MAINT_LOCKOUT_MS        300000UL    // 5 min
#define MAINT_PIN_MIN_LEN       4           // shorter, or the placeholder, is refused
#define MAINT_SILENCES_GAS      0           // 1 = gas alarm silent in maintenance too

// =========================================================
// 8. NETWORK AND MQTT  (must match mosquitto/config/acl and the server .env)
//
// The board joins the server PC's hotspot as an ordinary WiFi client and talks
// MQTT. It hosts no access point and no web server: there is nothing on it to
// connect to, which is the point.
//
// Joining is non-blocking. A missing hotspot or broker never stalls the loop:
// the sensors, LEDs and buzzer keep working offline, and reconnection backs off.
// =========================================================

#ifndef WIFI_JOIN_STATION
  #define WIFI_JOIN_STATION     1           // 0 = fully offline (bench / serial only)
#endif

#define MQTT_HOST               "10.42.0.1"         // this PC while it hosts the
                                                    // hotspot (NetworkManager always
                                                    // gives the host that address).
                                                    // Must be inside certs/server.crt,
                                                    // or TLS validation fails.
#define GROUP_ID                "g02"               // must match the Mosquitto ACL
#define MQTT_USERNAME           "esp32"             // ACL account name (kept as esp32)

#ifndef MQTT_USE_TLS
  #define MQTT_USE_TLS          1                   // 1 = port 8883 + CA pinning
#endif

#if MQTT_USE_TLS
  #define MQTT_PORT             8883
#else
  #define MQTT_PORT             1883
#endif

#define MQTT_KEEPALIVE_S        30
#define MQTT_CONNECT_TIMEOUT_MS 3000UL      // bounds how long a failed connect blocks
#define MQTT_RETRY_MIN_MS       2000UL      // reconnect backoff: doubles up to...
#define MQTT_RETRY_MAX_MS       60000UL     // ...this, so a dead broker costs ~nothing
#define STATE_HEARTBEAT_MS      5000UL      // state republished at least this often
#define STATE_FAST_MS           1000UL      // ...and this often during a countdown

// Commands arriving on the cmd topic:
#define CMD_MAX_BYTES           256         // longer is rejected unread
#define CMD_RATE_PER_S          5           // more than this per second is dropped

// TLS on an ESP8266 - the two things that actually bite:
//  a) HEAP. BearSSL wants a 16 kB receive buffer unless the broker negotiates
//     MFLN, and Mosquitto does not. We probe for MFLN and shrink when offered.
//  b) THE CLOCK. X.509 validation checks notBefore/notAfter; the board has no
//     RTC and an isolated hotspot has no NTP, so the clock is seeded from
//     BUILD_EPOCH. Bump it if certificates start reading as expired.
#define MQTT_TLS_RX_BUFFER      1024
#define MQTT_TLS_TX_BUFFER      1024
// Derived from __DATE__ at COMPILE TIME, never hardcoded.
//
// This was a hardcoded 1759708800 (2025-10-06). Certificates regenerated in
// 2026 were then "not yet valid" by 366 days, BearSSL refused the handshake,
// and the symptom was maddening: the TCP socket opened, Mosquitto logged
// "New connection", and no MQTT login ever followed. Deriving it from the
// build removes that whole class of failure.
//
// Two days are added so a certificate generated slightly AFTER the firmware
// was built is still inside its validity window. Being a couple of days ahead
// costs nothing against a one-year certificate.
//
// If TLS ever fails with a date error again: rebuild the firmware after
// regenerating the certificates.
constexpr int buildYear() {
  return (__DATE__[7] - '0') * 1000 + (__DATE__[8] - '0') * 100
       + (__DATE__[9] - '0') * 10   + (__DATE__[10] - '0');
}
constexpr int buildMonth() {
  return __DATE__[0] == 'J' && __DATE__[1] == 'a' ? 1
       : __DATE__[0] == 'F' ? 2
       : __DATE__[0] == 'M' && __DATE__[2] == 'r' ? 3
       : __DATE__[0] == 'A' && __DATE__[1] == 'p' ? 4
       : __DATE__[0] == 'M' ? 5
       : __DATE__[0] == 'J' && __DATE__[2] == 'n' ? 6
       : __DATE__[0] == 'J' ? 7
       : __DATE__[0] == 'A' ? 8
       : __DATE__[0] == 'S' ? 9
       : __DATE__[0] == 'O' ? 10
       : __DATE__[0] == 'N' ? 11 : 12;
}
constexpr int buildDay() {
  return (__DATE__[4] == ' ' ? 0 : (__DATE__[4] - '0') * 10) + (__DATE__[5] - '0');
}
// days_from_civil: the standard proleptic-Gregorian day count from 1970-01-01.
constexpr long daysFromCivil(long y, unsigned m, unsigned d) {
  return (y -= m <= 2,
          (y >= 0 ? y : y - 399) / 400 * 146097L
        + (long)((unsigned)(y - (y >= 0 ? y : y - 399) / 400 * 400) * 365
               + (unsigned)(y - (y >= 0 ? y : y - 399) / 400 * 400) / 4
               - (unsigned)(y - (y >= 0 ? y : y - 399) / 400 * 400) / 100
               + (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1)
        - 719468L);
}
#define BUILD_EPOCH ((unsigned long)((daysFromCivil(buildYear(), buildMonth(), buildDay()) + 2) * 86400L))

// Topics. Shape fixed by the Mosquitto ACL; only GROUP_ID should ever change.
#define TOPIC_PREFIX            "sentinelx/" GROUP_ID "/"
#define TOPIC_TELEMETRY         TOPIC_PREFIX "telemetry"   // readings, every 5 s
#define TOPIC_STATE             TOPIC_PREFIX "state"       // full state, retained
#define TOPIC_TEST              TOPIC_PREFIX "test"        // self-test progress, retained
#define TOPIC_TESTMETA          TOPIC_PREFIX "testmeta"    // self-test step list, retained
#define TOPIC_ALERTS            TOPIC_PREFIX "alerts"      // alarms + security events
#define TOPIC_STATUS            TOPIC_PREFIX "status"      // online / offline (LWT)
#define TOPIC_CMD               TOPIC_PREFIX "cmd"         // commands IN
#define TOPIC_VISION            TOPIC_PREFIX "vision"      // ce que la camera voit, IN

// =========================================================
// 9. SERIAL
//
// The serial console mirrors everything the dashboard sees, and is trusted:
// "maint on" there needs no PIN, because USB access to the board already
// means physical access to it.
// =========================================================

#define SERIAL_BAUD             115200
#define SERIAL_LOG_INTERVAL_MS  5000UL

// =========================================================
// Compile-time sanity checks
// =========================================================

#if ENABLE_MQTT && !WIFI_JOIN_STATION
  #error "ENABLE_MQTT needs WIFI_JOIN_STATION 1 - the broker is on the server's network."
#endif

#if ENABLE_PRESENCE && PRESENCE_SNIFFER && (SNIFF_WINDOW_MS >= SNIFF_PERIOD_MS)
  #error "SNIFF_WINDOW_MS must be well under SNIFF_PERIOD_MS."
#endif

#ifndef MAINT_PIN
  #error "MAINT_PIN missing from secrets.h - copy it from secrets.example.h."
#endif

// NOTE: pin collisions cannot be checked here. On the ESP8266 core D0..D8 are
// "static const uint8_t", not #defines, so the preprocessor treats them all as
// undefined and every `#if PIN_A == PIN_B` silently evaluates 0 == 0 as true.
// Those checks live as static_assert in src/main.cpp instead.
