// Standalone PIR test for the NodeMCU LoLin V3 (ESP8266MOD / ESP-12F).
// Serial Monitor: 115200 baud. No libraries, no WiFi, no credentials.
//
// Ported from an ESP32 version that used GPIO26 — a pin that does not exist on
// this chip. Matches the main firmware's pin map.
//
// Wiring:
//   PIR OUT -> D1 (GPIO5)       the MIDDLE pin of the three
//   PIR VCC -> 5 V from VU      NOT VIN: on the LoLin V3, VIN is an input to
//                               the onboard regulator and reads dead on USB power
//   PIR GND -> GND
//
// The HC-SR501's OUT swings 3.3 V even on a 5 V supply, so it drives D1 directly
// with no level shifting.
//
// TWO FAILURE MODES THAT LOOK IDENTICAL:
//   * A floating ESP8266 input reads a stable 1. Indistinguishable from a
//     sensor stuck on. If this prints MOTION immediately and never clears,
//     suspect the wire before the sensor.
//   * The module has no silkscreen and reversing VCC/GND gives exactly that
//     same stuck-on reading. Check the pinout against the board, not by feel.

#include <Arduino.h>

constexpr uint8_t PIR_PIN = D1;      // GPIO5
constexpr unsigned long WARMUP_S = 60;

int previousState = -1;

void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  delay(1000);

  Serial.println();
  Serial.println(F("--- Sentinel-X PIR test (ESP8266, D1/GPIO5) ---"));
  Serial.printf("Initial level: %d  (a stable 1 before warm-up usually means a "
                "floating pin or reversed VCC/GND)\n", digitalRead(PIR_PIN));
  Serial.println(F("The HC-SR501 false-trips for about a minute after power-up."));
  Serial.println(F("Keep still and wait."));

  for (unsigned long s = WARMUP_S; s > 0; s -= 10) {
    Serial.printf("  %lu s remaining...\n", s);
    delay(10000);
  }
  Serial.println(F("Ready. Move in front of the sensor."));
}

void loop() {
  const int state = digitalRead(PIR_PIN);
  if (state != previousState) {
    Serial.printf("[%6lus] %s\n", millis() / 1000,
                  state == HIGH ? "MOTION DETECTED (OUT = HIGH)"
                                : "sensor idle (OUT = LOW)");
    previousState = state;
  }
  delay(50);
}
