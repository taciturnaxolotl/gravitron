/*
 * Robot Tank Drive — Minimal Motor Firmware
 * Receives 5-byte binary frames (0xAB | left | right | buttons | CRC8)
 * Drives TB6612 motor driver directly.
 */

#include <Arduino.h>

// Pin definitions
#define PIN_PWMA    5
#define PIN_PWMB    6
#define PIN_AIN1    7
#define PIN_BIN1    8
#define PIN_STBY    3

// CRC8 Dallas/Maxim
static uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x80) crc = (crc << 1) ^ 0x31;
      else crc <<= 1;
    }
  }
  return crc;
}

// dir: true=forward, false=backward. speed: 0-255.
static void motor_write(bool dir, uint8_t speed, uint8_t pin_pwm, uint8_t pin_dir) {
  if (speed == 0) {
    analogWrite(pin_pwm, 0);
    return;
  }
  digitalWrite(pin_dir, dir ? HIGH : LOW);
  analogWrite(pin_pwm, speed);
}

static void tank_drive(int8_t left, int8_t right) {
  bool dirL, dirR;
  uint8_t spdL, spdR;

  if (left >= 0)  { dirL = HIGH; spdL = min((int16_t)left * 2, 255); }
  else            { dirL = LOW;  spdL = min((int16_t)-left * 2, 255); }

  if (right >= 0) { dirR = HIGH; spdR = min((int16_t)right * 2, 255); }
  else            { dirR = LOW;  spdR = min((int16_t)-right * 2, 255); }

  motor_write(dirL, spdL, PIN_PWMB, PIN_BIN1);
  motor_write(dirR, spdR, PIN_PWMA, PIN_AIN1);
}

static unsigned long last_packet_ms = 0;

void setup() {
  Serial.begin(115200);

  pinMode(PIN_PWMA, OUTPUT);
  pinMode(PIN_PWMB, OUTPUT);
  pinMode(PIN_AIN1, OUTPUT);
  pinMode(PIN_BIN1, OUTPUT);
  pinMode(PIN_STBY, OUTPUT);
  digitalWrite(PIN_STBY, LOW);
}

void loop() {
    // Always read serial regardless of watchdog state
    while (Serial.available() && Serial.peek() != 0xAB) Serial.read();

    if (Serial.available() >= 5) {
        Serial.read(); // consume 0xAB
        int8_t left     = Serial.read();
        int8_t right    = Serial.read();
        uint8_t buttons = Serial.read();
        uint8_t crc_rcvd = Serial.read();

        uint8_t payload[3] = {(uint8_t)left, (uint8_t)right, buttons};
        if (crc8(payload, 3) == crc_rcvd) {
            tank_drive(left, right);
            Serial.write((uint8_t)0xAC);
            last_packet_ms = millis();
        }
    }

    // Manage STBY once per iteration based on timeout
    if (millis() - last_packet_ms > 300) {
        tank_drive(0, 0);
        digitalWrite(PIN_STBY, LOW);
    } else {
        digitalWrite(PIN_STBY, HIGH);
    }
}