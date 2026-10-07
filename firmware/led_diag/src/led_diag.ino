// =====================================================================
// Sentinel-X - 3-LED channel diagnostic
//
// Standalone. No config.h, no libraries, no WiFi. 115200 baud.
//
// Built for one symptom: GREEN on D5/GPIO14 stays dark while YELLOW
// (D0/GPIO16) and RED (D8/GPIO15) behave normally, with a 220 R resistor
// fitted on all three.
//
// The electrical tests run at the top of every loop() pass, not just in
// setup(), so a serial capture started at any moment still sees a full set.
//
// ---------------------------------------------------------------------
// WHY EACH PIN NEEDS DIFFERENT EXPECTATIONS
// ---------------------------------------------------------------------
// The pull-up test is only meaningful on GPIO14, and reporting it for the
// other two would produce false alarms:
//
//   GPIO14 (D5)  ordinary GPIO with a real internal pull-up. INPUT_PULLUP
//                reading LOW means something external is holding it down.
//   GPIO16 (D0)  special RTC pin. It has NO internal pull-up at all, only
//                a pull-down. INPUT_PULLUP is not supported here, so the
//                test is skipped rather than misread.
//   GPIO15 (D8)  carries an onboard ~10k pull-down to keep boot valid.
//                INPUT_PULLUP reads LOW on a perfectly good board, because
//                the weak internal pull-up loses to that resistor. Skipped.
//
// GPIO14 also doubles as HSPI clock, but nothing here and nothing in the
// Sentinel-X firmware initialises SPI, so it is a plain GPIO in both.
// That is not the cause.
// =====================================================================

struct Channel {
  const char *name;
  const char *label;
  uint8_t     pin;
  uint8_t     gpio;
  bool        pullupMeaningful;
};

const Channel CH[] = {
  { "GREEN  (suspect)", "D5", D5, 14, true  },
  { "YELLOW (working)", "D0", D0, 16, false },
  { "RED    (working)", "D8", D8, 15, false },
};
const uint8_t CH_COUNT = sizeof(CH) / sizeof(CH[0]);

// GPIO2 / D4 carries TWO things, both active-low: the module's own blue LED,
// and (once wired) the buzzer's PNP switch. So the blue LED previews the
// buzzer pattern even with no buzzer attached yet.
const uint8_t BUZZER = D4;       // GPIO2, ACTIVE-LOW via PNP high-side switch
const uint8_t ONBOARD_LED = 2;   // same pin, the ESP-12F's own blue LED

// As built: D4 -> 100R -> buzzer -> GND, so HIGH sounds it (active-HIGH).
// Flip BUZZER_SOUNDS_ON to LOW if the buzzer is ever rewired to 3V3.
const uint8_t BUZZER_SOUNDS_ON = HIGH;
inline void buzz(bool on) {
  digitalWrite(BUZZER, on ? BUZZER_SOUNDS_ON : !BUZZER_SOUNDS_ON);
}

uint32_t pass = 0;

void allLow() {
  for (uint8_t i = 0; i < CH_COUNT; i++) {
    pinMode(CH[i].pin, OUTPUT);
    digitalWrite(CH[i].pin, LOW);
  }
}

// Drive a level, let it settle, then read what the pin is actually sitting at.
int driveAndRead(const Channel &c, bool high) {
  pinMode(c.pin, OUTPUT);
  digitalWrite(c.pin, high ? HIGH : LOW);
  delayMicroseconds(400);
  return digitalRead(c.pin);
}

void electricalTests() {
  Serial.println(F("\n=== ELECTRICAL TESTS ========================="));

  for (uint8_t i = 0; i < CH_COUNT; i++) {
    const Channel &c = CH[i];
    Serial.printf("\n  %s   %s / GPIO%u\n", c.name, c.label, c.gpio);

    const int hi = driveAndRead(c, true);
    const int lo = driveAndRead(c, false);

    Serial.printf("    T1  drive HIGH -> reads %-4s   %s\n",
                  hi ? "HIGH" : "LOW", hi == HIGH ? "ok" : "<<< MISMATCH");
    Serial.printf("    T2  drive LOW  -> reads %-4s   %s\n",
                  lo ? "HIGH" : "LOW", lo == LOW ? "ok" : "<<< MISMATCH");

    int pu = -1;
    if (c.pullupMeaningful) {
      pinMode(c.pin, INPUT_PULLUP);
      delay(2);
      pu = digitalRead(c.pin);
      Serial.printf("    T3  INPUT_PULLUP -> reads %-4s %s\n",
                    pu ? "HIGH" : "LOW",
                    pu == HIGH ? "ok (nothing dragging it down)"
                               : "<<< HELD LOW externally");
    } else {
      Serial.printf("    T3  INPUT_PULLUP -> skipped (GPIO%u has no usable\n", c.gpio);
      Serial.println(F("                     internal pull-up; see header note)"));
    }

    pinMode(c.pin, INPUT);
    delay(2);
    const int fl = digitalRead(c.pin);
    Serial.printf("    T4  floating     -> reads %-4s\n", fl ? "HIGH" : "LOW");

    // Verdict
    Serial.print(F("    => "));
    if (hi != HIGH) {
      Serial.println(F("PIN CANNOT GO HIGH. Short to GND, or a load far"));
      Serial.println(F("       beyond what a GPIO can source. Chip-side fault."));
    } else if (lo != LOW) {
      Serial.println(F("PIN CANNOT GO LOW. Something feeds it from 3V3."));
    } else if (c.pullupMeaningful && pu == LOW) {
      Serial.println(F("Switches ok, but held LOW when released ->"));
      Serial.println(F("       external short to ground on this net."));
    } else {
      Serial.println(F("PIN IS HEALTHY at the chip. It drives both levels"));
      Serial.println(F("       cleanly. Any dark LED is downstream: wrong"));
      Serial.println(F("       header pin, open joint, or dead LED."));
    }
  }
  allLow();
}

void visualPhase() {
  Serial.println(F("\n=== VISUAL PHASE - watch the LEDs ============"));
  for (uint8_t i = 0; i < CH_COUNT; i++) {
    allLow();
    digitalWrite(CH[i].pin, HIGH);
    Serial.printf("  >>> %s is being driven HIGH for 3 s"
                  "  (only this one should light)\n", CH[i].name);
    delay(3000);
  }
  allLow();
  Serial.println(F("  >>> all three driven LOW for 3 s"
                   "       (all three should be dark)"));
  delay(3000);

  for (uint8_t i = 0; i < CH_COUNT; i++) digitalWrite(CH[i].pin, HIGH);
  Serial.println(F("  >>> all three driven HIGH for 3 s"
                   "      (all three should light)"));
  delay(3000);
  allLow();
}

// Active-low, so the expectations are inverted relative to the LEDs:
// driving GPIO2 LOW sounds the buzzer and lights the blue LED.
void buzzerTests() {
  Serial.println(F("\n=== BUZZER  D4 / GPIO2  (ACTIVE-HIGH) ========"));
  Serial.println(F("Wired D4 -> 100R -> buzzer -> GND, so HIGH sounds it."));
  Serial.println(F("The onboard BLUE LED is on the same pin but is"));
  Serial.println(F("active-LOW, so it is lit when the buzzer is SILENT."));

  pinMode(BUZZER, OUTPUT);
  buzz(false);
  delayMicroseconds(400);
  const int offLvl = digitalRead(BUZZER);
  buzz(true);
  delayMicroseconds(400);
  const int onLvl = digitalRead(BUZZER);
  buzz(false);

  Serial.printf("    silent -> pin reads %-4s  %s\n", offLvl ? "HIGH" : "LOW",
                offLvl == !BUZZER_SOUNDS_ON ? "ok" : "<<< MISMATCH");
  Serial.printf("    sound  -> pin reads %-4s  %s\n", onLvl ? "HIGH" : "LOW",
                onLvl == BUZZER_SOUNDS_ON ? "ok" : "<<< MISMATCH");

  // The two firmware patterns, reproduced exactly so they can be judged by ear
  // before committing to them.
  Serial.println(F("\n  RED pattern: burst of 3 beeps then a pause, 6 s"));
  unsigned long t0 = millis();
  while (millis() - t0 < 6000) {
    const unsigned long t = (millis() - t0) % (3 * 240 + 700);
    buzz(t < 3 * 240 && (t % 240) < 120);
    delay(5);
  }
  buzz(false);

  Serial.println(F("  gap, 1.5 s"));
  delay(1500);

  Serial.println(F("  YELLOW pattern: 100 ms chirp every 4 s, 9 s"));
  t0 = millis();
  while (millis() - t0 < 9000) {
    buzz(((millis() - t0) % 4000) < 100);
    delay(5);
  }
  buzz(false);

  Serial.println(F("  GREEN: silent (nothing should sound), 2 s"));
  delay(2000);
}

void setup() {
  Serial.begin(115200);
  delay(400);
  allLow();
  pinMode(ONBOARD_LED, OUTPUT);

  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F("  Sentinel-X  -  3-LED channel diagnostic"));
  Serial.println(F("=============================================="));
  Serial.printf("chip id   : %08X\n", ESP.getChipId());
  Serial.printf("build     : %s %s\n", __DATE__, __TIME__);
  Serial.printf("reset     : %s\n", ESP.getResetReason().c_str());
  Serial.println(F("expected  : pin -> 220R -> LED -> GND, active-high"));
  Serial.println(F("            green D5/GPIO14, yellow D0/GPIO16, red D8/GPIO15"));
  Serial.println(F("            buzzer D4/GPIO2, ACTIVE-LOW via PNP"));

  // Proof-of-life independent of any external wiring.
  for (uint8_t i = 0; i < 3; i++) {
    digitalWrite(ONBOARD_LED, LOW);  delay(120);
    digitalWrite(ONBOARD_LED, HIGH); delay(120);
  }
}

void loop() {
  pass++;
  Serial.printf("\n\n################ PASS %lu ################\n", (unsigned long)pass);
  electricalTests();
  visualPhase();
  buzzerTests();
}
