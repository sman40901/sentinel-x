// =====================================================================
// Sentinel-X - sensor diagnostic
//
// Standalone. 115200 baud. No config.h, no secrets.h, no WiFi.
//
// Chases two faults seen on the real board:
//   * the DHT22 on D2 fails EVERY read
//   * A0 reports ~750/1023, which the firmware scales to 4.8 V at the
//     sensor - above the 3.3 V rail, so either the MQ is genuinely
//     warming up or A0 is floating
//
// ---------------------------------------------------------------------
// THE DHT CHECKS, AND WHY EACH ONE
// ---------------------------------------------------------------------
// 1. IDLE LINE LEVEL. A healthy DHT data line idles HIGH, held there by a
//    pull-up. Reading LOW while idle means the sensor is unpowered, the
//    data pin is shorted, or VCC/GND are swapped. Reading HIGH only with
//    the internal pull-up enabled means there is NO EXTERNAL PULL-UP:
//    the ESP8266's internal one (~45k) is weak and marginal for a DHT,
//    which is a classic cause of "every read fails" with correct wiring.
//
// 2. DHT22 *AND* DHT11 DECODE. The housing colour is the giveaway: white
//    is usually DHT22, blue usually DHT11. Decoding a DHT22's bytes with
//    DHT11 scaling gives a plausible-looking but wrong ~2.3 C / 2.8 %RH,
//    so a "working" DHT11 read is itself evidence of the wrong type.
//
// 3. BOTH CANDIDATE PINS. D2 is where the firmware expects it, but a
//    one-pin slip is exactly what the green LED turned out to be, so D5
//    is tried too. D5 is otherwise the green LED, which will simply light
//    during that test - harmless.
//
// A0 NOTE: A0 has no internal pull-up and nothing to hold it anywhere, so
// a floating A0 drifts and reads noisy. A rock-steady value across many
// samples suggests something IS driving it; a wandering one suggests not.
// =====================================================================

#include <Arduino.h>
#include <DHT.h>
#include <Wire.h>

const uint8_t DHT_PRIMARY = D2;   // GPIO4, where the firmware expects it
const uint8_t DHT_ALT     = D5;   // GPIO14, in case of a one-pin slip
const uint8_t PIR_PIN     = D1;   // GPIO5
const uint8_t IMU_SDA     = D6;
const uint8_t IMU_SCL     = D7;

uint32_t pass = 0;

void lineLevelCheck(uint8_t pin, const char *label) {
  pinMode(pin, INPUT);              // no pull: what is the line doing alone?
  delay(5);
  const int bare = digitalRead(pin);
  pinMode(pin, INPUT_PULLUP);       // now with the weak internal pull-up
  delay(5);
  const int pulled = digitalRead(pin);

  Serial.printf("    %s idle: bare=%s  with internal pull-up=%s\n",
                label, bare ? "HIGH" : "LOW", pulled ? "HIGH" : "LOW");

  if (bare == HIGH) {
    Serial.println(F("      -> an external pull-up is present. Good."));
  } else if (pulled == HIGH) {
    Serial.println(F("      -> NO EXTERNAL PULL-UP. The internal one is weak"));
    Serial.println(F("         (~45k) and marginal for a DHT. Add 4.7k-10k"));
    Serial.println(F("         from the data line to 3.3 V."));
  } else {
    Serial.println(F("      -> line stuck LOW even when pulled up: sensor"));
    Serial.println(F("         unpowered, data pin shorted to GND, or"));
    Serial.println(F("         VCC/GND swapped on the module."));
  }
}

void tryDht(uint8_t pin, uint8_t type, const char *pinLabel, const char *typeLabel) {
  DHT dht(pin, type);
  dht.begin();
  delay(2200);                      // a DHT refuses reads closer than 2 s
  const float t = dht.readTemperature();
  const float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {
    Serial.printf("    %s as %-5s -> FAILED\n", pinLabel, typeLabel);
    return;
  }
  Serial.printf("    %s as %-5s -> %.1f C  /  %.1f %%RH", pinLabel, typeLabel, t, h);
  if (type == DHT11 && t < 10.0f && h < 10.0f) {
    Serial.print(F("   <- suspiciously low: this is the"
                   " DHT11-decoding-a-DHT22 signature"));
  }
  Serial.println();
}

void dhtChecks() {
  Serial.println(F("\n=== DHT (temperature / humidity) ============="));
  lineLevelCheck(DHT_PRIMARY, "D2 ");
  lineLevelCheck(DHT_ALT,     "D5 ");
  Serial.println(F("  decode attempts (each needs >2 s, be patient):"));
  tryDht(DHT_PRIMARY, DHT22, "D2", "DHT22");
  tryDht(DHT_PRIMARY, DHT11, "D2", "DHT11");
  tryDht(DHT_ALT,     DHT22, "D5", "DHT22");
  tryDht(DHT_ALT,     DHT11, "D5", "DHT11");
}

void gasChecks() {
  Serial.println(F("\n=== GAS (A0) ================================="));

  // 24 samples over ~1.2 s. A floating pin wanders; a driven one does not.
  int lo = 1024, hi = -1;
  long sum = 0;
  for (uint8_t i = 0; i < 24; i++) {
    const int v = analogRead(A0);
    sum += v;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    delay(50);
  }
  const int mean = (int)(sum / 24);
  const int spread = hi - lo;

  // The firmware's scaling: A0 sees half of the sensor's AO through the
  // 68k+68k divider, so it multiplies back up by 2.
  const float atPin = mean * 3.3f / 1023.0f;

  Serial.printf("    raw mean %d/1023   min %d   max %d   spread %d\n",
                mean, lo, hi, spread);
  Serial.printf("    => %.2f V at A0, so %.2f V at the sensor's AO (x2 divider)\n",
                atPin, atPin * 2.0f);

  if (mean <= 2) {
    Serial.println(F("    -> pinned at 0: no supply to the MQ, or AO shorted to GND."));
  } else if (mean >= 1020) {
    Serial.println(F("    -> pinned at full scale: divider missing or miswired."));
    Serial.println(F("       AO can reach 5 V and A0 must never see more than 3.3 V."));
  } else if (spread > 40) {
    Serial.println(F("    -> wandering a lot. Consistent with a FLOATING A0,"));
    Serial.println(F("       i.e. nothing actually connected to it."));
  } else {
    Serial.println(F("    -> steady, so something is driving it. If the figure at AO"));
    Serial.println(F("       reads above ~3.3 V the MQ is still heating; an MQ runs"));
    Serial.println(F("       high for the first minutes and settles. Re-check after"));
    Serial.println(F("       3 minutes before blaming the wiring."));
  }
  Serial.println(F("    (reminder: this is never ppm. Without R0 calibrated in"));
  Serial.println(F("     clean air, only the change against a baseline means anything.)"));
}

void pirChecks() {
  Serial.println(F("\n=== PIR (D1) ================================="));
  pinMode(PIR_PIN, INPUT);
  int high = 0;
  for (uint8_t i = 0; i < 40; i++) {
    if (digitalRead(PIR_PIN) == HIGH) high++;
    delay(50);
  }
  Serial.printf("    HIGH in %d of 40 samples over 2 s\n", high);
  if (high == 40) {
    Serial.println(F("    -> stuck HIGH. Either still warming up (first minute),"));
    Serial.println(F("       or VCC/GND reversed, or the pin is floating. A floating"));
    Serial.println(F("       ESP8266 input reads a stable 1, which looks identical"));
    Serial.println(F("       to a permanently triggered sensor. OUT is the MIDDLE pin."));
  } else if (high == 0) {
    Serial.println(F("    -> idle. Wave at it; it should go HIGH."));
  } else {
    Serial.println(F("    -> changing state, so it is alive."));
  }
}

void i2cChecks() {
  Serial.println(F("\n=== I2C (D6 = SDA, D7 = SCL) ================="));
  Wire.begin(IMU_SDA, IMU_SCL);
  // 100 kHz, not 400: the ESP8266 bit-bangs I2C and the MPU goes silent faster.
  Wire.setClock(100000);
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("    device at 0x%02X", addr);
      if (addr == 0x68 || addr == 0x69) {
        Wire.beginTransmission(addr);
        Wire.write(0x75);                 // WHO_AM_I
        if (Wire.endTransmission(false) == 0 && Wire.requestFrom(addr, (uint8_t)1) == 1) {
          const uint8_t who = Wire.read();
          Serial.printf("  WHO_AM_I=0x%02X%s", who,
                        who == 0x70 ? " (MPU-6500, no magnetometer)" :
                        who == 0x71 ? " (MPU-9250)" : " (unexpected)");
        }
      }
      Serial.println();
      found++;
    }
  }
  if (!found) {
    Serial.println(F("    nothing responding. Check SDA/SCL are not swapped,"));
    Serial.println(F("    that VCC is on the 3.3 V island, and that the clock"));
    Serial.println(F("    is 100 kHz - 400 kHz silences the MPU entirely."));
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F("  Sentinel-X  -  sensor diagnostic"));
  Serial.println(F("=============================================="));
  Serial.printf("chip id : %08X\n", ESP.getChipId());
  Serial.printf("build   : %s %s\n", __DATE__, __TIME__);
  Serial.println(F("Note: the D5 DHT attempts will light the green LED."));
}

void loop() {
  pass++;
  Serial.printf("\n\n############ PASS %lu ############\n", (unsigned long)pass);
  dhtChecks();
  gasChecks();
  pirChecks();
  i2cChecks();
  Serial.println(F("\n--- repeating in 5 s ---"));
  delay(5000);
}
