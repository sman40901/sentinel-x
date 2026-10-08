// =====================================================================
// I2C bus check - why does nothing answer on D6/D7?
//
// A plain address scan says "nothing there" for three very different
// faults. This separates them.
//
// 1. ARE THE LINES PULLED UP?
//    An I2C module powers its own pull-up resistors. Read the pins as
//    plain inputs with NO internal pull-up:
//      both HIGH  -> external pull-ups present, so the module HAS POWER
//                    and both wires reach it. A silent chip then means a
//                    dead chip or the wrong address.
//      either LOW -> that line is shorted, or held down.
//      only HIGH with the internal pull-up -> NO external pull-ups:
//                    the module is NOT powered, or that wire is not
//                    actually connected.
//
// 2. ARE SDA AND SCL SWAPPED?
//    Scans both orientations. Swapped lines look exactly like an empty
//    bus, and it is the easiest mistake to make when reseating.
//
// 3. IS IT AT A DIFFERENT ADDRESS?
//    Scans the whole 1..126 range, not just 0x68.
//
// 100 kHz throughout: the ESP8266 bit-bangs I2C in software and the
// MPU-6500 stops answering entirely at 400 kHz.
// =====================================================================

#include <Arduino.h>
#include <Wire.h>

const uint8_t PIN_A = D6;      // normally SDA
const uint8_t PIN_B = D7;      // normally SCL

void lineReport(uint8_t pin, const char *name) {
  pinMode(pin, INPUT);          // no pull: what is the line doing on its own?
  delay(5);
  const int bare = digitalRead(pin);
  pinMode(pin, INPUT_PULLUP);
  delay(5);
  const int pulled = digitalRead(pin);
  Serial.printf("    %s  bare=%-4s  internal pull-up=%-4s  -> ",
                name, bare ? "HIGH" : "LOW", pulled ? "HIGH" : "LOW");
  if (bare == HIGH) {
    Serial.println(F("external pull-up PRESENT (module powered)"));
  } else if (pulled == HIGH) {
    Serial.println(F("NO external pull-up (module unpowered or wire open)"));
  } else {
    Serial.println(F("STUCK LOW (shorted to GND)"));
  }
  pinMode(pin, INPUT);
}

uint8_t scan(uint8_t sda, uint8_t scl, const char *label) {
  Wire.begin(sda, scl);
  Wire.setClock(100000);
  delay(20);
  Serial.printf("  %s\n", label);
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("    device at 0x%02X", addr);
      if (addr == 0x68 || addr == 0x69) {
        Wire.beginTransmission(addr);
        Wire.write(0x75);                       // WHO_AM_I
        if (Wire.endTransmission(false) == 0 && Wire.requestFrom(addr, (uint8_t)1) == 1) {
          const uint8_t who = Wire.read();
          Serial.printf("  WHO_AM_I=0x%02X%s", who,
                        who == 0x70 ? " (MPU-6500)" : who == 0x71 ? " (MPU-9250)" : " (?)");
        }
      }
      Serial.println();
      found++;
    }
  }
  if (!found) Serial.println(F("    nothing"));
  return found;
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F("  I2C bus check - D6 / D7"));
  Serial.println(F("=============================================="));
}

void loop() {
  Serial.println(F("\n=== 1. Line levels (is the module powered?) ==="));
  lineReport(PIN_A, "D6");
  lineReport(PIN_B, "D7");

  Serial.println(F("\n=== 2. Address scan, both orientations ==="));
  const uint8_t normal  = scan(PIN_A, PIN_B, "SDA=D6, SCL=D7 (expected wiring):");
  const uint8_t swapped = scan(PIN_B, PIN_A, "SDA=D7, SCL=D6 (swapped):");

  Serial.println(F("\n=== verdict ==="));
  if (normal)        Serial.println(F("  Bus OK as wired. The firmware should find it."));
  else if (swapped)  Serial.println(F("  SDA AND SCL ARE SWAPPED. Exchange the two wires."));
  else               Serial.println(F("  Nothing on either orientation - see the line levels above."));

  Serial.println(F("\n--- repeating in 4 s, you can move wires and watch ---"));
  delay(4000);
}
