// MPU-6500 over I2C, raw register access, used here as a TAMPER sensor:
// if the box gets picked up, tilted or struck, that is a security event.
//
// Deliberately not using an MPU9250 library. The part sold as an MPU-9250 is
// actually an MPU-6500 — WHO_AM_I (0x75) reads 0x70, and most libraries check
// for 0x71 and refuse to initialise. The accel/gyro blocks are register
// compatible, so direct access just works.
//
// There is NO magnetometer on this part: pitch and roll are available, heading
// is not. Don't add a compass feature expecting it to work.
//
// Wiring: VCC -> 3.3 V island, GND -> GND, SDA -> D6, SCL -> D7.
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "config.h"

static const uint8_t IMU_ADDR          = 0x68;

static const uint8_t IMU_SMPLRT_DIV    = 0x19;
static const uint8_t IMU_CONFIG        = 0x1A;
static const uint8_t IMU_GYRO_CONFIG   = 0x1B;
static const uint8_t IMU_ACCEL_CONFIG  = 0x1C;
static const uint8_t IMU_ACCEL_XOUT_H  = 0x3B;
static const uint8_t IMU_PWR_MGMT_1    = 0x6B;
static const uint8_t IMU_WHO_AM_I      = 0x75;

// At the full-scale settings selected in imuBegin(): +/-2 g and +/-250 deg/s.
static const float IMU_ACCEL_LSB_PER_G   = 16384.0f;
static const float IMU_GYRO_LSB_PER_DPS  = 131.0f;

static const float RAD_TO_DEGREES = 57.2957795f;

struct ImuReading {
  float ax, ay, az;     // g
  float gx, gy, gz;     // deg/s
  float pitch, roll;    // degrees
  float celsius;        // the die's own sensor, not room temperature
  float magnitude;      // total accel vector in g; ~1.0 at rest
};

inline bool imuWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

inline bool imuReadBytes(uint8_t reg, uint8_t *dst, uint8_t count) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(IMU_ADDR, count) != count) {
    return false;
  }
  for (uint8_t i = 0; i < count; i++) {
    dst[i] = Wire.read();
  }
  return true;
}

// Returns the WHO_AM_I value, or 0 if the device never answered.
inline uint8_t imuBegin() {
  Wire.begin(PIN_IMU_SDA, PIN_IMU_SCL);

  // 100 kHz, NOT 400 kHz. The ESP8266 bit-bangs I2C in software and the MPU
  // stops answering entirely at the faster rate. This is not a margin thing —
  // 400 kHz gives you a silent bus and a long debugging session.
  Wire.setClock(100000);
  delay(10);

  // One retry: the very first transaction after Wire.begin() sometimes lands
  // before the bus has settled.
  uint8_t who = 0;
  if (!imuReadBytes(IMU_WHO_AM_I, &who, 1)) {
    delay(50);
    if (!imuReadBytes(IMU_WHO_AM_I, &who, 1)) {
      return 0;
    }
  }

  imuWrite(IMU_PWR_MGMT_1, 0x80);   // reset
  delay(100);
  imuWrite(IMU_PWR_MGMT_1, 0x01);   // wake, clock from the X gyro PLL
  delay(10);
  imuWrite(IMU_CONFIG, 0x03);       // 41 Hz low-pass, calms the readings down
  imuWrite(IMU_SMPLRT_DIV, 0x04);   // 200 Hz sample rate
  imuWrite(IMU_GYRO_CONFIG, 0x00);  // +/-250 deg/s
  imuWrite(IMU_ACCEL_CONFIG, 0x00); // +/-2 g

  return who;
}

inline bool imuRead(ImuReading *out) {
  uint8_t raw[14];
  if (!imuReadBytes(IMU_ACCEL_XOUT_H, raw, sizeof(raw))) {
    return false;
  }

  const int16_t axr = (int16_t)((raw[0]  << 8) | raw[1]);
  const int16_t ayr = (int16_t)((raw[2]  << 8) | raw[3]);
  const int16_t azr = (int16_t)((raw[4]  << 8) | raw[5]);
  const int16_t tr  = (int16_t)((raw[6]  << 8) | raw[7]);
  const int16_t gxr = (int16_t)((raw[8]  << 8) | raw[9]);
  const int16_t gyr = (int16_t)((raw[10] << 8) | raw[11]);
  const int16_t gzr = (int16_t)((raw[12] << 8) | raw[13]);

  out->ax = axr / IMU_ACCEL_LSB_PER_G;
  out->ay = ayr / IMU_ACCEL_LSB_PER_G;
  out->az = azr / IMU_ACCEL_LSB_PER_G;
  out->gx = gxr / IMU_GYRO_LSB_PER_DPS;
  out->gy = gyr / IMU_GYRO_LSB_PER_DPS;
  out->gz = gzr / IMU_GYRO_LSB_PER_DPS;

  // Per the MPU-6500 register map.
  out->celsius = (tr / 333.87f) + 21.0f;

  // Gravity gives two of the three angles. Yaw needs a magnetometer this part
  // does not have.
  out->pitch = atan2(-out->ax, sqrt(out->ay * out->ay + out->az * out->az)) * RAD_TO_DEGREES;
  out->roll  = atan2(out->ay, out->az) * RAD_TO_DEGREES;

  out->magnitude = sqrt(out->ax * out->ax + out->ay * out->ay + out->az * out->az);

  return true;
}

// --- tamper helpers -------------------------------------------------------

// Angle in degrees between the current gravity vector and a reference one.
//
// This replaces comparing pitch and roll against a reference separately, which
// breaks near pitch +/-90: roll is then atan2 of two near-zero numbers and
// swings on sensor noise alone. Measured on the bench with the board perfectly
// still (|a| steady at 1.01 g, pitch steady to 0.8 deg) roll wandered over
// 10.3 deg - most of the way to a false tamper alarm. The angle between two
// vectors has no singularity in any orientation, so how the box is mounted no
// longer matters.
inline float imuAngleFromRef(const ImuReading &r, const float ref[3]) {
  const float dot = r.ax * ref[0] + r.ay * ref[1] + r.az * ref[2];
  const float mag = sqrtf((r.ax * r.ax + r.ay * r.ay + r.az * r.az)
                        * (ref[0] * ref[0] + ref[1] * ref[1] + ref[2] * ref[2]));
  if (mag <= 0.0f) {
    return 0.0f;
  }
  float c = dot / mag;
  if (c > 1.0f) c = 1.0f;          // rounding can push it a hair past 1
  if (c < -1.0f) c = -1.0f;
  return acosf(c) * RAD_TO_DEGREES;
}
