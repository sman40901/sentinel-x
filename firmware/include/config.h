// =========================================================
// Sentinel-X — ALL tunable settings live in this one file.
//
// Target: NodeMCU LoLin V3 / ESP8266MOD (ESP-12F)
// PlatformIO env: nodemcuv2
//
// Secrets (WiFi passwords, MQTT password, CA cert) are NOT here.
// They live in include/secrets.h, which git ignores.
// See secrets.example.h.
// =========================================================
#pragma once

#include "secrets.h"

// =========================================================
// 1. FEATURE FLAGS — turn whole subsystems on/off
//
// Each 0 here frees both flash and heap. If the board gets
// unstable, switch things off from the bottom of this list up:
// the sniffer and TLS are by far the most expensive.
// =========================================================

// Each is wrapped in #ifndef so a PlatformIO env can override it with
// -DENABLE_x=0 without editing this file. See the extra envs in platformio.ini.

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
  #define ENABLE_IMU            1   // MPU-6500, used for tamper/tilt detection
#endif
#ifndef ENABLE_LEDS
  #define ENABLE_LEDS           1   // the 3 status LEDs
#endif
#ifndef ENABLE_BUZZER
  #define ENABLE_BUZZER         1   // READ the PIN_BUZZER wiring notes first
#endif
#ifndef ENABLE_WEB_DASHBOARD
  #define ENABLE_WEB_DASHBOARD  1   // page + JSON API served by the board itself
#endif
#ifndef ENABLE_PRESENCE
  #define ENABLE_PRESENCE       1   // WiFi device detection (see section 5)
#endif
#ifndef ENABLE_MQTT
  #define ENABLE_MQTT           1   // publish to the Docker broker (see section 6)
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
#define GAS_INTERVAL_MS         2000UL
#define IMU_INTERVAL_MS         500UL
#define TELEMETRY_INTERVAL_MS   5000UL      // how often we publish to MQTT
#define MQTT_RETRY_MS           5000UL
#define LED_REFRESH_MS          50UL        // drives the blink patterns

// Warm-up windows. Readings before these are published but flagged, and never
// raise an alert.
#define PIR_WARMUP_MS           60000UL     // HC-SR501 false-trips for ~1 min
#define GAS_WARMUP_MS           180000UL    // MQ heater needs ~3 min to mean anything
                                            // (a brand-new sensor wants 24-48 h burn-in)

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

#define GAS_BASELINE_SAMPLES    10          // averaged after warm-up to set the baseline

// Gas thresholds are a RISE ABOVE THE SETTLED BASELINE, in volts at the sensor.
// An MQ gives no absolute ppm without calibrating R0 in clean air, so a delta
// against a baseline is the only honest signal. Tune on site.
#define GAS_WARN_DELTA_V        0.35f       // -> early warning (yellow)
#define GAS_CRIT_DELTA_V        0.90f       // -> intruder/critical (red) + alert

// Tamper detection: the box being picked up, tilted or knocked.
//
// TILT_WARN_DEG is a deviation from the orientation the box was in AT STARTUP,
// not an absolute angle. The enclosure is not necessarily level - on the bench
// it sat at roll -89 - and comparing against 0 degrees made it report permanent
// tamper and hold the system at YELLOW forever. Same principle as the gas
// baseline: what matters is the change, not the absolute reading.
#define TILT_WARN_DEG           25.0f       // deviation from the startup reference
#define IMU_BASELINE_MS         5000UL      // settle this long before capturing it
#define IMU_SHOCK_G             1.8f        // total accel spike = impact

// =========================================================
// 5. PRESENCE DETECTION  (WiFi device detection)
//
// Two independent signals, combined into one "presence score":
//
//   a) ASSOCIATED CLIENTS — devices actually joined to our AP.
//      Free, continuous, exact. Only sees devices that connect.
//
//   b) PROBE SNIFFER — promiscuous-mode capture of 802.11 management
//      frames from phones that are NOT connected to us. This is the
//      "through walls" signal.
//
// READ THIS BEFORE TUNING (b):
//
//   * The ESP8266 CANNOT sniff and host the AP at the same time. Entering
//     promiscuous mode tears the AP down, so every sniff window kicks
//     dashboard clients off and drops the MQTT link for its duration.
//     Keep SNIFF_WINDOW_MS short and SNIFF_PERIOD_MS long.
//
//   * Modern phones RANDOMIZE their MAC in probe requests (iOS 8+,
//     Android 10+). One phone can produce many addresses in a minute, so
//     the unique-MAC count is an ACTIVITY LEVEL, not a device count. We
//     report randomized and stable MACs separately for that reason.
//
//   * This captures identifiers from the air. Fine for your own workshop;
//     know what it is before pointing it at a public space.
// =========================================================

#ifndef PRESENCE_SNIFFER
  #define PRESENCE_SNIFFER      1           // 0 = associated clients only (no AP drops)
#endif

#define SNIFF_PERIOD_MS         60000UL     // how often a sniff window opens
#define SNIFF_WINDOW_MS         3000UL      // how long the AP stays down for it
#define SNIFF_CHANNELS          {1, 6, 11}  // 2.4 GHz non-overlapping channels
#define SNIFF_DWELL_MS          300UL       // time per channel before hopping

#define PRESENCE_TABLE_SIZE     40          // max MACs remembered (RAM: 10 B each)
#define PRESENCE_EXPIRE_MS      300000UL    // a MAC unseen this long is forgotten

// Score = associated clients * ASSOC_WEIGHT + sniffed MACs * SNIFF_WEIGHT.
// Associated devices are weighted higher because they are real and unique.
#define PRESENCE_ASSOC_WEIGHT   3
#define PRESENCE_SNIFF_WEIGHT   1

#define PRESENCE_WARN_SCORE     3           // -> early warning (yellow)
#define PRESENCE_CRIT_SCORE     10          // -> contributes to intruder (red)

// Only count sniffed frames stronger than this. Raising it shrinks the
// detection radius, which is how you stop seeing the neighbours.
#define SNIFF_MIN_RSSI          -80         // dBm

// =========================================================
// 6. THREAT STATE MACHINE -> which LED is lit
//
//   GREEN  "validation"     all sensors healthy, nothing seen
//   YELLOW "early warning"  something nearby but unconfirmed:
//                             presence score >= PRESENCE_WARN_SCORE
//                             gas delta >= GAS_WARN_DELTA_V
//                             box tilted past TILT_WARN_DEG
//   RED    "intruder"       confirmed:
//                             PIR motion (after warm-up)
//                             gas delta >= GAS_CRIT_DELTA_V
//                             presence score >= PRESENCE_CRIT_SCORE
//
// Green also blinks instead of holding solid when a sensor is unhealthy,
// so a dead DHT is visible without opening the dashboard.
// =========================================================

#define INTRUDER_HOLD_MS        15000UL     // red stays lit this long after the last trip
#define WARNING_HOLD_MS         10000UL     // yellow likewise
#define ALERT_COOLDOWN_MS       30000UL     // min gap between MQTT alerts of one type

// An MQTT {"led":"..."} command seizes the LEDs for this long, then hands them
// back to the state machine. Bounded on purpose: a forgotten test command must
// not leave the box permanently lying about what it can see.
#define LED_OVERRIDE_MS         10000UL

// --- buzzer patterns -------------------------------------------------------
//
// Deliberately different shapes rather than different volumes, because an
// active buzzer has exactly one pitch and one loudness. Rhythm is the only
// channel available, so red and yellow must be distinguishable by ear alone.
//
// All non-blocking: the loop never waits on the buzzer.
// RED: a BURST of beeps, then a pause. Bursts rather than a plain on/off
// duty cycle because a fast duty cycle does not read as a rhythm at all -
// at ~3 Hz an active buzzer's own tone plus the chopping just sounds like one
// continuous warble. Measured on the bench: 150/150 ms was indistinguishable
// from a solid drone, while beeps around 120 ms separated by a clear pause are
// unmistakable. Keep BUZZER_RED_BEEP_MS well above ~100 ms for this reason.
#define BUZZER_RED_BEEPS        3        // beeps per burst
#define BUZZER_RED_BEEP_MS      120UL    // length of each beep
#define BUZZER_RED_GAP_MS       120UL    // silence between beeps in a burst
#define BUZZER_RED_PAUSE_MS     700UL    // silence after the burst

// YELLOW: one short chirp, rarely. Present but easy to live with.
#define BUZZER_YELLOW_ON_MS     100UL
#define BUZZER_YELLOW_PERIOD_MS 4000UL
// Green is silent. A device that beeps when nothing is wrong gets taped over.

// An MQTT {"buzzer":1} command holds the buzzer for this long, then hands it
// back to the state machine. Same reasoning as LED_OVERRIDE_MS: a forgotten
// test command must not leave the box sounding forever.
#define BUZZER_OVERRIDE_MS      5000UL

// =========================================================
// 7. NETWORK
//
// AP_STA = host our own dashboard AP *and* join the server's network for
// MQTT. Both radios share one channel (the station's wins), which is normal.
// Set WIFI_JOIN_STATION to 0 for a standalone box with no broker.
// =========================================================

#ifndef WIFI_JOIN_STATION
  #define WIFI_JOIN_STATION     1           // 0 = AP only, fully standalone
#endif
#define WIFI_STA_TIMEOUT_MS     20000UL     // then carry on; sensors don't need the network
#define WEB_PORT                80
#define MDNS_HOSTNAME           "sentinel-x"    // http://sentinel-x.local/

// =========================================================
// 8. MQTT  (must match mosquitto/config/acl and the server .env)
// =========================================================

#define MQTT_HOST               "192.168.137.1"     // the server PC on the AP
#define GROUP_ID                "g02"               // must match the Mosquitto ACL
#define MQTT_USERNAME           "esp32"             // account name in the ACL — kept as
                                                    // "esp32" so the ACL/.env still match,
                                                    // even though this is an ESP8266
#ifndef MQTT_USE_TLS
  #define MQTT_USE_TLS          1                   // 1 = port 8883 + CA pinning
#endif

#if MQTT_USE_TLS
  #define MQTT_PORT             8883
#else
  #define MQTT_PORT             1883
#endif

#define MQTT_KEEPALIVE_S        30

// --- TLS on an ESP8266: the two things that actually bite ---
//
// a) HEAP. BearSSL wants a 16 kB receive buffer unless the broker negotiates
//    MFLN, and Mosquitto (OpenSSL) does not. That is ~22 kB of a ~40 kB heap,
//    on top of the web server and the presence table. We probe for MFLN and
//    shrink to the sizes below when it is offered. Watch "heap" on the
//    dashboard; if it dips under ~8 kB you are about to get random resets.
//    The fix is MQTT_USE_TLS 0, or ENABLE_PRESENCE 0.
//
// b) THE CLOCK. X.509 validation checks notBefore/notAfter, and the board has
//    no RTC and no internet on an isolated AP, so NTP cannot help. We seed the
//    clock from BUILD_EPOCH below. Bump it if certs start reading as expired.
#define MQTT_TLS_RX_BUFFER      1024
#define MQTT_TLS_TX_BUFFER      1024
#define BUILD_EPOCH             1759708800UL    // 2025-10-06 — seeds X509 time

// Topics. Shape fixed by the Mosquitto ACL; only GROUP_ID should ever change.
#define TOPIC_TELEMETRY         "sentinelx/" GROUP_ID "/telemetry"
#define TOPIC_ALERTS            "sentinelx/" GROUP_ID "/alerts"
#define TOPIC_STATUS            "sentinelx/" GROUP_ID "/status"
#define TOPIC_CMD               "sentinelx/" GROUP_ID "/cmd"

// =========================================================
// 9. SERIAL
// =========================================================

#define SERIAL_BAUD             115200
#define SERIAL_LOG_INTERVAL_MS  5000UL

// =========================================================
// Compile-time sanity checks — these catch the mistakes that
// cost an afternoon on the bench.
// =========================================================

#if ENABLE_MQTT && !WIFI_JOIN_STATION
  #error "ENABLE_MQTT needs WIFI_JOIN_STATION 1 — the broker is on the server's network."
#endif

#if ENABLE_PRESENCE && PRESENCE_SNIFFER && (SNIFF_WINDOW_MS >= SNIFF_PERIOD_MS)
  #error "SNIFF_WINDOW_MS must be well under SNIFF_PERIOD_MS or the AP is never up."
#endif

// NOTE: pin collisions cannot be checked here. On the ESP8266 core D0..D8 are
// "static const uint8_t", not #defines, so the preprocessor treats them all as
// undefined and every `#if PIN_A == PIN_B` silently evaluates 0 == 0 as true.
// Those checks live as static_assert in src/main.cpp instead, where the real
// constants are visible.

// BUZZER_ACTIVE_LOW 0 on GPIO2 used to be a hard #error here, because a load to
// GND pulls against a strap pin that wants to be HIGH at reset. It is now a
// deliberate, measured choice on this build: it boots, and the trade-off is
// written up in section 2. Left as a note rather than an error so the option
// stays available, but read that section before changing the buzzer.
