// =====================================================================
// Sentinel-X - pin hunt
//
// Standalone. No config.h, no libraries, no WiFi. 115200 baud.
//
// Problem: the green LED is dark when D5/GPIO14 is driven either way,
// but glows on GPIO14's reset pull-up. Contradictory - so the assumption
// that it is on D5 is probably wrong. This walks every safe GPIO and
// drives it, both polarities, until the green LED lights.
//
// ---------------------------------------------------------------------
// YOU DO NOT NEED THE SERIAL CONSOLE FOR THIS
// ---------------------------------------------------------------------
// Before each pin is tested, the RED LED (known working) blinks a COUNT.
// Count the red blinks, then watch green:
//
//     red blinks 5 times -> pause -> green lights  =>  it is on pin #5
//
// The numbering is printed below and is fixed:
//     1 = D0 / GPIO16        5 = D5 / GPIO14  (the assumed one)
//     2 = D2 / GPIO4         6 = D6 / GPIO12
//     3 = D3 / GPIO0         7 = D7 / GPIO13
//     4 = D4 / GPIO2         8 = D8 / GPIO15  (red itself)
//
// Phase A drives the pin HIGH (finds normal active-high wiring).
// Phase B drives it LOW while the rest are HIGH (finds reversed wiring).
//
// ---------------------------------------------------------------------
// D1 / GPIO5 IS DELIBERATELY NOT TESTED
// ---------------------------------------------------------------------
// That is the PIR's OUT line. If the sensor is still connected it is
// driving that pin, and driving it from this end too puts two push-pull
// outputs in contention, which can damage either. If green is not found
// below, unplug the PIR first and I will add D1 to the sweep.
//
// Step 8 tests D8, which is the red LED itself - so there the counter and
// the LED under test are the same. Expect red to blink 8 times and then
// stay solid. That step is still valid.
// =====================================================================

struct Pin { uint8_t idx; const char *label; uint8_t pin; uint8_t gpio; };

const Pin PINS[] = {
  { 1, "D0", D0, 16 },
  { 2, "D2", D2,  4 },
  { 3, "D3", D3,  0 },
  { 4, "D4", D4,  2 },
  { 5, "D5", D5, 14 },
  { 6, "D6", D6, 12 },
  { 7, "D7", D7, 13 },
  { 8, "D8", D8, 15 },
};
const uint8_t N = sizeof(PINS) / sizeof(PINS[0]);

const uint8_t RED = D8;          // the known-good LED, used as the counter
const unsigned long HOLD_MS = 2000;

uint32_t sweep = 0;

void setAll(bool high) {
  for (uint8_t i = 0; i < N; i++) {
    pinMode(PINS[i].pin, OUTPUT);
    digitalWrite(PINS[i].pin, high ? HIGH : LOW);
  }
}

// Blink the count on red. Skipped when red is the pin under test, because
// the two would fight.
void blinkCount(uint8_t n, uint8_t pinUnderTest) {
  if (pinUnderTest == RED) {
    // Still give a countable marker, just on the pin itself.
    for (uint8_t i = 0; i < n; i++) {
      digitalWrite(RED, HIGH); delay(130);
      digitalWrite(RED, LOW);  delay(170);
    }
    delay(500);
    return;
  }
  pinMode(RED, OUTPUT);
  for (uint8_t i = 0; i < n; i++) {
    digitalWrite(RED, HIGH); delay(130);
    digitalWrite(RED, LOW);  delay(170);
  }
  delay(500);
}

void phase(bool activeHigh) {
  Serial.printf("\n=== PHASE %s: driving each pin %s ===\n",
                activeHigh ? "A" : "B",
                activeHigh ? "HIGH (normal wiring)" : "LOW (reversed wiring)");

  for (uint8_t i = 0; i < N; i++) {
    const Pin &p = PINS[i];

    setAll(!activeHigh);           // rest idle
    blinkCount(p.idx, p.pin);

    setAll(!activeHigh);
    pinMode(p.pin, OUTPUT);
    digitalWrite(p.pin, activeHigh ? HIGH : LOW);

    Serial.printf("  [%u] %s / GPIO%-2u  driven %-4s for %lu ms"
                  "   <- green lighting NOW means it is on this pin\n",
                  p.idx, p.label, p.gpio, activeHigh ? "HIGH" : "LOW", HOLD_MS);
    delay(HOLD_MS);
    setAll(!activeHigh);
    delay(700);
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);
  setAll(false);

  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F("  Sentinel-X  -  pin hunt"));
  Serial.println(F("=============================================="));
  Serial.printf("chip id : %08X\n", ESP.getChipId());
  Serial.printf("build   : %s %s\n", __DATE__, __TIME__);
  Serial.println(F("Count the RED blinks, then watch GREEN."));
  Serial.println(F("  1=D0/16  2=D2/4  3=D3/0  4=D4/2"));
  Serial.println(F("  5=D5/14  6=D6/12 7=D7/13 8=D8/15"));
  Serial.println(F("D1/GPIO5 skipped on purpose (PIR OUT contention)."));
}

void loop() {
  sweep++;
  Serial.printf("\n\n############ SWEEP %lu ############\n", sweep);
  phase(true);
  phase(false);
  Serial.println(F("\n--- sweep complete, repeating in 3 s ---"));
  delay(3000);
}
