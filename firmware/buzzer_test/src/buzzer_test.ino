// =====================================================================
// Sentinel-X - buzzer polarity test
//
// Standalone. 115200 baud. Symptom being chased: the buzzer drones
// continuously instead of following a rhythm.
//
// The earlier diagnostic already proved GPIO2 switches correctly at the
// chip (drive HIGH reads HIGH, drive LOW reads LOW), so the pin is fine
// and the fault is in the buzzer branch.
//
// States are 4 seconds long and announced, so there is nothing to
// misread. GPIO2 also drives the module's onboard BLUE LED, active-low,
// and that LED is wired on the module and therefore always correct. Use
// it as the reference for what the pin is really doing.
//
//   blue LED ON  while buzzer SILENT  -> active-low: CORRECT
//   blue LED ON  while buzzer SOUNDS  -> inverted: buzzer is active-high
//   buzzer SOUNDS in BOTH states      -> not switched by the pin at all
//   buzzer SILENT in BOTH states      -> open branch, or no supply
//
// ---------------------------------------------------------------------
// WHAT "SOUNDS IN BOTH STATES" MEANS
// ---------------------------------------------------------------------
// The buzzer is getting power regardless of the pin. Usual causes:
//   * PNP emitter and collector swapped - it conducts whatever the base does
//   * base resistor missing, or the base tied to GND instead of GPIO2
//   * buzzer wired straight across 3V3 and GND, bypassing the transistor
//   * an NPN fitted where the PNP should be, wired so it is always on
// =====================================================================

const uint8_t BUZZER = D4;      // GPIO2, also the onboard blue LED
const unsigned long STATE_MS = 4000;

uint32_t cycle = 0;

void setup() {
  Serial.begin(115200);
  delay(400);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, HIGH);   // active-low: HIGH is the intended OFF state

  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F("  Sentinel-X  -  buzzer polarity test"));
  Serial.println(F("=============================================="));
  Serial.printf("build : %s %s\n", __DATE__, __TIME__);
  Serial.println(F("GPIO2 / D4. 4 s per state. Blue LED = reference."));
  Serial.println(F("Report WHICH state the buzzer sounds in."));
}

void loop() {
  cycle++;
  Serial.printf("\n---------------- cycle %lu ----------------\n",
                (unsigned long)cycle);

  digitalWrite(BUZZER, HIGH);
  Serial.println(F("GPIO2 = HIGH  for 4 s"));
  Serial.println(F("   active-low wiring  -> SILENT, blue LED dark"));
  Serial.printf("   pin reads %s\n", digitalRead(BUZZER) ? "HIGH" : "LOW");
  delay(STATE_MS);

  digitalWrite(BUZZER, LOW);
  Serial.println(F("GPIO2 = LOW   for 4 s"));
  Serial.println(F("   active-low wiring  -> SOUNDING, blue LED lit"));
  Serial.printf("   pin reads %s\n", digitalRead(BUZZER) ? "HIGH" : "LOW");
  delay(STATE_MS);

  // Slow enough to count by ear. If the buzzer does not change at all here,
  // it is not being switched by the pin.
  digitalWrite(BUZZER, HIGH);
  Serial.println(F("1 Hz toggle for 8 s - you should hear 8 distinct beeps"));
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(BUZZER, LOW);  delay(500);
    digitalWrite(BUZZER, HIGH); delay(500);
  }
  Serial.println(F("toggle done, back to HIGH (silent)"));
  delay(1500);
}
