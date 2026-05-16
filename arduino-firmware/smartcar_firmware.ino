/*
 * ELEGOO Smart Robot Car V4.0 — Clean Firmware
 *
 * Single-file Arduino UNO sketch. Direct serial control at 9600 baud.
 * No ESP32, no JSON, no app protocol. Just simple text commands.
 *
 * Hardware:
 *   TB6612 motor driver on pins 5,6,7,8 (STBY=3)
 *   HC-SR04 ultrasonic on pins 13(TRIG), 12(ECHO)
 *   MPU6050 gyro on I2C (A4/A5)
 *   3x IR trackers on A0(R), A1(M), A2(L)
 *   2x SG90 servos on pins 10(Z), 11(Y)
 *   WS2812 LED on pin 4
 *   IR receiver on pin 9
 *   Mode button on pin 2
 *   Battery voltage on A3
 *
 * Dependencies (Arduino Library Manager):
 *   FastLED, MPU6050, I2Cdev, IRremote, Servo (built-in)
 */

#include <avr/wdt.h>
#include <Servo.h>
#include <Wire.h>
#include <FastLED.h>
#include <I2Cdev.h>
#include <MPU6050.h>
#include <IRremote.h>

// ─── Pin Definitions ──────────────────────────────────────────
// Motors (TB6612)
#define PIN_PWMA    5
#define PIN_PWMB    6
#define PIN_AIN1    7
#define PIN_BIN1    8
#define PIN_STBY    3

// Ultrasonic (HC-SR04)
#define PIN_TRIG    13
#define PIN_ECHO    12

// IR Line Trackers (ITR20001)
#define PIN_IR_L    A2
#define PIN_IR_M    A1
#define PIN_IR_R    A0

// Servos
#define PIN_SERVO_Z 10
#define PIN_SERVO_Y 11

// RGB LED (WS2812)
#define PIN_LED     4
#define NUM_LEDS    1

// IR Receiver
#define PIN_IRRECV  9

// Voltage divider
#define PIN_VOLTAGE A3

// Mode button (INT0)
#define PIN_KEY     2

// ─── Motor Direction Constants ────────────────────────────────
#define DIR_FWD     true
#define DIR_BACK    false
#define DIR_COAST   3

// ─── Global Objects ───────────────────────────────────────────
Servo servo;
MPU6050 mpu;
IRrecv irrecv(PIN_IRRECV);
decode_results ir_results;
CRGB leds[NUM_LEDS];

// ─── MPU6050 Yaw State ────────────────────────────────────────
float    gz_offset = 0;
float    yaw_angle = 0;
unsigned long last_yaw_update = 0;

// ─── Mode Button ──────────────────────────────────────────────
volatile uint8_t key_value = 0;

// ─── Forward Declarations ─────────────────────────────────────
void motors_control(bool dirA, uint8_t spdA, bool dirB, uint8_t spdB);
void motors_stop();
void car_move(int dir, uint8_t speed);
void car_stop();
uint16_t ultrasonic_read();
float battery_read();
void servo_write(uint8_t which, int angle); // which: 1=Z, 2=Y
void led_set(uint8_t r, uint8_t g, uint8_t b);
void led_off();
void yaw_update();
void yaw_calibrate();
int  ir_read(int which); // 0=L, 1=M, 2=R


// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(9600);

  // Motors
  pinMode(PIN_PWMA, OUTPUT);
  pinMode(PIN_PWMB, OUTPUT);
  pinMode(PIN_AIN1, OUTPUT);
  pinMode(PIN_BIN1, OUTPUT);
  pinMode(PIN_STBY, OUTPUT);
  digitalWrite(PIN_STBY, LOW);

  // Ultrasonic
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  // IR trackers
  pinMode(PIN_IR_L, INPUT);
  pinMode(PIN_IR_M, INPUT);
  pinMode(PIN_IR_R, INPUT);

  // Battery
  pinMode(PIN_VOLTAGE, INPUT);

  // LED
  FastLED.addLeds<NEOPIXEL, PIN_LED>(leds, NUM_LEDS);
  FastLED.setBrightness(20);
  FastLED.clear(true);

  // Servos: center both
  servo.attach(PIN_SERVO_Z, 500, 2400);
  servo.write(90);
  delay(500);
  servo.detach();
  servo.attach(PIN_SERVO_Y, 500, 2400);
  servo.write(90);
  delay(500);
  servo.detach();

  // MPU6050
  Wire.begin();
  mpu.initialize();
  if (mpu.testConnection()) {
    yaw_calibrate();
    Serial.println(F("{\\\"mpu6050\\\":true}"));
  } else {
    Serial.println(F("{\\\"mpu6050\\\":false}"));
  }

  // IR receiver
  irrecv.enableIRIn();

  // Mode button
  pinMode(PIN_KEY, INPUT_PULLUP);
  attachInterrupt(0, key_isr, FALLING);

  // Watchdog (2s)
  wdt_enable(WDTO_2S);

  Serial.println(F("READY"));
}

// ═══════════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  wdt_reset();

  // Always update yaw
  yaw_update();

  // Handle serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    handle_command(cmd);
  }

  // Handle IR remote
  if (irrecv.decode(&ir_results)) {
    handle_ir();
    irrecv.resume();
  }
}

// ═══════════════════════════════════════════════════════════════
//  COMMAND HANDLER
// ═══════════════════════════════════════════════════════════════
void handle_command(String cmd) {
  cmd.toLowerCase();

  // ── Motor commands ──
  if (cmd == "f" || cmd == "forward") {
    car_move(1, 200);
    Serial.println(F("OK forward"));
  }
  else if (cmd == "b" || cmd == "back") {
    car_move(2, 200);
    Serial.println(F("{\"ok\":\"back\"}"));
  }
  else if (cmd == "l" || cmd == "left") {
    car_move(3, 200);
    Serial.println(F("{\"ok\":\"left\"}"));
  }
  else if (cmd == "r" || cmd == "right") {
    car_move(4, 200);
    Serial.println(F("{\"ok\":\"right\"}"));
  }
  else if (cmd == "fl" || cmd == "forwardleft") {
    car_move(5, 200);
    Serial.println(F("{\"ok\":\"forward-left\"}"));
  }
  else if (cmd == "fr" || cmd == "forwardright") {
    car_move(7, 200);
    Serial.println(F("{\"ok\":\"forward-right\"}"));
  }
  else if (cmd == "bl" || cmd == "backleft") {
    car_move(6, 200);
    Serial.println(F("{\"ok\":\"back-left\"}"));
  }
  else if (cmd == "br" || cmd == "backright") {
    car_move(8, 200);
    Serial.println(F("{\"ok\":\"back-right\"}"));
  }
  else if (cmd == "s" || cmd == "stop") {
    car_stop();
    Serial.println(F("{\"ok\":\"stop\"}"));
  }

  // ── Motor raw: m <left_dir> <left_spd> <right_dir> <right_spd> ──
  else if (cmd.startsWith("m ")) {
    int a_dir, a_spd, b_dir, b_spd;
    int n = sscanf(cmd.c_str(), "m %d %d %d %d", &a_dir, &a_spd, &b_dir, &b_spd);
    if (n == 4) {
      bool dirA = (a_dir == 1) ? DIR_FWD : DIR_BACK;
      bool dirB = (b_dir == 1) ? DIR_FWD : DIR_BACK;
      motors_control(dirA, constrain(a_spd, 0, 255), dirB, constrain(b_spd, 0, 255));
      Serial.println(F("{\"ok\":\"motors\"}"));
    }
  }

  // ── Servo: sv <which> <angle> ──
  else if (cmd.startsWith("sv ")) {
    int which, angle;
    if (sscanf(cmd.c_str(), "sv %d %d", &which, &angle) == 2) {
      servo_write(which, constrain(angle, 0, 180));
      Serial.println(F("{\"ok\":\"servo\"}"));
    }
  }

  // ── Ultrasonic: us ──
  else if (cmd == "us") {
    uint16_t dist = ultrasonic_read();
    Serial.print(F("{\"distance\":"));
    Serial.print(dist);
    Serial.println(F("}"));
  }

  // ── IR sensors: ir ──
  else if (cmd == "ir") {
    Serial.print(F("{\"ir\":{\"l\":"));
    Serial.print(ir_read(0));
    Serial.print(F(",\"m\":"));
    Serial.print(ir_read(1));
    Serial.print(F(",\"r\":"));
    Serial.print(ir_read(2));
    Serial.println(F("}}"));
  }

  // ── Battery: bat ──
  else if (cmd == "bat") {
    Serial.print(F("{\"battery\":"));
    Serial.print(battery_read());
    Serial.println(F("}"));
  }

  // ── LED: led <r> <g> <b> ──
  else if (cmd.startsWith("led ")) {
    int r, g, b;
    if (sscanf(cmd.c_str(), "led %d %d %d", &r, &g, &b) == 3) {
      led_set(constrain(r, 0, 255), constrain(g, 0, 255), constrain(b, 0, 255));
      Serial.println(F("{\"ok\":\"led\"}"));
    }
  }

  // ── LED off: ledoff ──
  else if (cmd == "ledoff") {
    led_off();
    Serial.println(F("{\"ok\":\"ledoff\"}"));
  }

  // ── Speed: speed <0-255> ── (sets speed for f/b/l/r commands)
  else if (cmd.startsWith("speed ")) {
    int spd;
    if (sscanf(cmd.c_str(), "speed %d", &spd) == 1) {
      move_speed = constrain(spd, 0, 255);
      Serial.print(F("{\"speed\":"));
      Serial.print(move_speed);
      Serial.println(F("}"));
    }
  }

  // ── Yaw: yaw ──
  else if (cmd == "yaw") {
    Serial.print(F("{\"yaw\":"));
    Serial.print(yaw_angle, 2);
    Serial.println(F("}"));
  }

  // ── Calibrate: cal ──
  else if (cmd == "cal") {
    yaw_calibrate();
    Serial.println(F("{\"ok\":\"calibrated\"}"));
  }

  // ── Help ──
  else if (cmd == "?" || cmd == "help") {
    Serial.println(F("Commands:"));
    Serial.println(F("  f/b/l/r    — forward/back/left/right"));
    Serial.println(F("  fl/fr/bl/br — diagonal movement"));
    Serial.println(F("  s/stop     — stop"));
    Serial.println(F("  speed N    — set move speed (0-255)"));
    Serial.println(F("  m dirA spdA dirB spdB — raw motor control"));
    Serial.println(F("  sv which angle — servo (which:1=Z,2=Y)"));
    Serial.println(F("  us         — ultrasonic distance"));
    Serial.println(F("  ir         — IR tracker readings"));
    Serial.println(F("  bat        — battery voltage"));
    Serial.println(F("  led R G B  — set LED color"));
    Serial.println(F("  ledoff     — turn LED off"));
    Serial.println(F("  yaw        — read yaw angle"));
    Serial.println(F("  cal        — calibrate gyro"));
  }

  else {
    Serial.print(F("{\\\"unknown\\\":\\\""));
    Serial.print(cmd);
    Serial.println(F("\\\"}"));
  }
}

static uint8_t move_speed = 200;

// ═══════════════════════════════════════════════════════════════
//  IR REMOTE HANDLER
// ═══════════════════════════════════════════════════════════════
void handle_ir() {
  // Elegoo remote A codes
  switch (ir_results.value) {
    case 16736925: car_move(1, move_speed); break; // up
    case 16754775: car_move(2, move_speed); break; // down
    case 16720605: car_move(3, move_speed); break; // left
    case 16761405: car_move(4, move_speed); break; // right
    case 16712445: car_stop();                break; // OK = stop
    default: break;
  }
}

// ═══════════════════════════════════════════════════════════════
//  MOTOR CONTROL
// ═══════════════════════════════════════════════════════════════
void motors_control(bool dirA, uint8_t spdA, bool dirB, uint8_t spdB) {
  digitalWrite(PIN_STBY, HIGH);

  // Motor A (right)
  if (dirA == DIR_FWD) {
    digitalWrite(PIN_AIN1, HIGH);
    analogWrite(PIN_PWMA, spdA);
  } else if (dirA == DIR_BACK) {
    digitalWrite(PIN_AIN1, LOW);
    analogWrite(PIN_PWMA, spdA);
  } else {
    analogWrite(PIN_PWMA, 0);
  }

  // Motor B (left)
  if (dirB == DIR_FWD) {
    digitalWrite(PIN_BIN1, HIGH);
    analogWrite(PIN_PWMB, spdB);
  } else if (dirB == DIR_BACK) {
    digitalWrite(PIN_BIN1, LOW);
    analogWrite(PIN_PWMB, spdB);
  } else {
    analogWrite(PIN_PWMB, 0);
  }
}

void motors_stop() {
  digitalWrite(PIN_STBY, LOW);
  analogWrite(PIN_PWMA, 0);
  analogWrite(PIN_PWMB, 0);
}

// dir: 1=forward  2=back  3=left  4=right
//      5=left-fwd 6=left-back  7=right-fwd  8=right-back
void car_move(int dir, uint8_t speed) {
  switch (dir) {
    case 1: // forward (yaw corrected)
      car_move_yaw(1, speed);
      break;
    case 2: // back (yaw corrected)
      car_move_yaw(2, speed);
      break;
    case 3: // left
      motors_control(DIR_FWD, speed, DIR_BACK, speed);
      break;
    case 4: // right
      motors_control(DIR_BACK, speed, DIR_FWD, speed);
      break;
    case 5: // left-forward
      motors_control(DIR_FWD, speed, DIR_FWD, speed / 2);
      break;
    case 6: // left-back
      motors_control(DIR_BACK, speed, DIR_BACK, speed / 2);
      break;
    case 7: // right-forward
      motors_control(DIR_FWD, speed / 2, DIR_FWD, speed);
      break;
    case 8: // right-back
      motors_control(DIR_BACK, speed / 2, DIR_BACK, speed);
      break;
  }
}

// Yaw-corrected straight-line drive
void car_move_yaw(int direction, uint8_t speed) {
  static float yaw_ref = 0;
  static uint8_t last_dir = 0;

  if (last_dir != direction) {
    yaw_ref = yaw_angle;
    last_dir = direction;
  }

  float error = yaw_angle - yaw_ref;
  int correction = error * 10;  // Kp = 10

  int speedR = constrain(speed - correction, 10, 255);
  int speedL = constrain(speed + correction, 10, 255);

  if (direction == 1) { // forward
    motors_control(DIR_FWD, speedR, DIR_FWD, speedL);
  } else { // back
    motors_control(DIR_BACK, speedL, DIR_BACK, speedR);
  }
}

void car_stop() {
  motors_control(DIR_COAST, 0, DIR_COAST, 0);
}

// ═══════════════════════════════════════════════════════════════
//  SENSORS
// ═══════════════════════════════════════════════════════════════
uint16_t ultrasonic_read() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  uint16_t dist = pulseIn(PIN_ECHO, HIGH) / 58;
  return (dist > 150) ? 150 : dist;  // clamp to 150cm
}

int ir_read(int which) {
  switch (which) {
    case 0: return analogRead(PIN_IR_L);
    case 1: return analogRead(PIN_IR_M);
    case 2: return analogRead(PIN_IR_R);
    default: return 0;
  }
}

float battery_read() {
  float v = analogRead(PIN_VOLTAGE) * 0.0375;
  return v * 1.08;  // 8% compensation
}

// ═══════════════════════════════════════════════════════════════
//  MPU6050 YAW
// ═══════════════════════════════════════════════════════════════
void yaw_update() {
  unsigned long now = millis();
  float dt = (now - last_yaw_update) / 1000.0;
  last_yaw_update = now;

  int16_t gz = mpu.getRotationZ();
  float gyroz = -(gz - gz_offset) / 131.0 * dt;
  if (fabs(gyroz) < 0.05) gyroz = 0;  // deadband
  yaw_angle += gyroz;
}

void yaw_calibrate() {
  long sum = 0;
  for (int i = 0; i < 100; i++) {
    sum += mpu.getRotationZ();
    delay(5);
  }
  gz_offset = sum / 100;
  yaw_angle = 0;
}

// ═══════════════════════════════════════════════════════════════
//  SERVO
// ═══════════════════════════════════════════════════════════════
void servo_write(uint8_t which, int angle) {
  uint8_t pin = (which == 1) ? PIN_SERVO_Z : PIN_SERVO_Y;
  servo.attach(pin, 500, 2400);
  servo.write(constrain(angle, 0, 180));
  delay(450);
  servo.detach();
}

// ═══════════════════════════════════════════════════════════════
//  LED
// ═══════════════════════════════════════════════════════════════
void led_set(uint8_t r, uint8_t g, uint8_t b) {
  leds[0] = CRGB(r, g, b);
  FastLED.show();
}

void led_off() {
  leds[0] = CRGB::Black;
  FastLED.show();
}

// ═══════════════════════════════════════════════════════════════
//  MODE BUTTON ISR
// ═══════════════════════════════════════════════════════════════
void key_isr() {
  static unsigned long last = 0;
  if (millis() - last > 500) {
    key_value = (key_value + 1) % 4;
    last = millis();
  }
}
