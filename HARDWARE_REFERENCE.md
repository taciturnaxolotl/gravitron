# ELEGOO Smart Robot Car V4.0 — Hardware Reference

## Architecture Overview

The car has **two microcontrollers** connected via a single UART link:

```
┌─────────────────────┐     UART (9600 baud)     ┌─────────────────────┐
│    ESP32 CAM        │◄─────────────────────────►│   Arduino UNO R3    │
│  (ESP32-WROVER or   │    TX/RX cross-connected  │   (ATmega328P)      │
│   ESP32-S3-WROOM-1) │                           │                     │
│                     │                           │                     │
│  • WiFi AP mode     │                           │  • Motor driver     │
│  • Camera stream    │                           │    (TB6612)         │
│  • TCP socket bridge│                           │  • Ultrasonic HC-SR04│
│  • Heartbeat        │                           │  • IR trackers x3   │
│                     │                           │  • MPU6050 (GY-521) │
│                     │                           │  • SG90 servos x2   │
│                     │                           │  • RGB LED (WS2812) │
│                     │                           │  • IR receiver      │
│                     │                           │  • Voltage divider  │
└─────────────────────┘                           └─────────────────────┘
```

The ESP32 is the **wireless gateway** and the UNO is the **real-time motor/sensor controller**. All user commands flow: Phone App → WiFi → ESP32 TCP socket → UART → Arduino UNO.

---

## 1. ESP32 Camera Module

### 1.1 Two hardware variants

| Variant | Board | PSRAM | Flash | Notes |
|---------|-------|-------|-------|-------|
| V1 (2022) | ESP32-WROVER | 4MB | 4MB | Uses `CAMERA_MODEL_M5STACK_WIDE` pinout |
| V2 (2023) | ESP32-S3-WROOM-1 | 8MB OPI | 8MB | Uses `CAMERA_MODEL_ESP32S3_EYE` pinout |

### 1.2 ESP32 ↔ Arduino UART link

The physical connection between the two boards is a **3-wire UART** (there's no HW flow control):

**ESP32-WROVER (V1):**
- `RXD2` = GPIO 33 → Arduino pin 1 (TX)
- `TXD2` = GPIO 4  → Arduino pin 0 (RX)
- GND common

**ESP32-S3 (V2):**
- `RXD2` = GPIO 3  → Arduino pin 1 (TX)
- `TXD2` = GPIO 40 → Arduino pin 0 (RX)
- GND common

Both sides run `Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2)`. The Arduino side is `Serial.begin(9600)` on hardware UART (pins 0/1).

### 1.3 ESP32 WiFi

The ESP32 runs in **AP (Access Point) mode** — it creates its own WiFi network. No router needed.

- **SSID**: `ELEGOO-` + last 6 hex digits of MAC (e.g., `ELEGOO-A1B2C3D4E5F6`)
- **Password**: empty (open network)
- **IP**: `192.168.4.1` (fixed, always)
- **Tx power**: 19.5dBm max

A phone connects to this WiFi, then the Elegoo app talks to `192.168.4.1:100`.

### 1.4 ESP32 TCP socket bridge (the critical logic)

The ESP32 listens on **port 100** for exactly **one TCP client**. This is the pass-through bridge:

1. Phone app connects to `192.168.4.1:100`
2. ESP32 reads frames from the TCP socket (delimited by `{...}`)
3. Filters heartbeat: `{Heartbeat}` messages stay internal
4. Everything else is forwarded verbatim to `Serial2` (to Arduino)
5. Any response from `Serial2` (Arduino) is forwarded back to the TCP client
6. If WiFi client count drops to 0, ESP32 sends `{"N":100}` (stop command) to Arduino
7. Heartbeat every 1 second — after 3 missed heartbeats, disconnect client and send stop

The ESP32 has **zero knowledge** of the protocol content. It's a dumb serial-TCP pipe plus heartbeat and connection watchdog.

### 1.5 Camera

- OV2640 sensor (2MP)
- JPEG streaming at `http://192.168.4.1/stream` (MJPEG multipart)
- Single capture at `http://192.168.4.1/capture`
- Camera settings via `http://192.168.4.1/control?var=X&val=Y` (framerate, quality, brightness, etc.)
- Web UI at `http://192.168.4.1/` with face detection/recognition included

### 1.6 ESP32 pin usage (non-camera)

| GPIO (WROVER) | GPIO (S3) | Function |
|---------------|-----------|----------|
| 13 | 46 | Status LED (H=on, L=off, blinks when no client connected) |
| 33 | 3 | UART RX from Arduino |
| 4 | 40 | UART TX to Arduino |

---

## 2. Arduino UNO — Motor & Sensor Controller

### 2.1 Pin Map

#### Motors — TB6612 driver

| Arduino Pin | TB6612 Pin | Description |
|-------------|------------|-------------|
| 5 | PWMA | Right motor PWM (0-255) |
| 6 | PWMB | Left motor PWM (0-255) |
| 7 | AIN1 | Right motor direction |
| 8 | BIN1 | Left motor direction |
| 3 | STBY | Motor enable (HIGH=on) |

#### Ultrasonic — HC-SR04

| Arduino Pin | Sensor Pin |
|-------------|------------|
| 13 | TRIG |
| 12 | ECHO |

Range: ~2-150 cm (clamped in code). Formula: `distance = pulseIn(ECHO, HIGH) / 58`

#### IR Line Trackers — ITR20001 (x3)

| Arduino Pin | Sensor |
|-------------|--------|
| A2 | Left (L) |
| A1 | Middle (M) |
| A0 | Right (R) |

Analog read. The line tracking mode uses thresholds: detection occurs when analog value is between 250–850. Values >950 on all three sensors mean "car lifted off ground" (used as a safety stop).

#### MPU6050 (GY-521) — 6-axis IMU

I2C connection (A4=SDA, A5=SCL on UNO). The code only uses **Z-axis gyro** for yaw correction during straight-line driving. Integrates `gz` over `dt`, with a deadband of ±0.05 to filter drift.

`Wire.begin()` is called during init. Device ID 0x68 is expected.

#### Servos — SG90 (x2)

| Arduino Pin | Servo | Range |
|-------------|-------|-------|
| 10 | Z-axis (pan) | 10°–170° (mapped from raw 1–17 in code) |
| 11 | Y-axis (tilt) | 30°–110° (mapped from raw 3–11 in code) |

Initialized to 90° center. Servo PWM 500–2400µs range.

#### RGB LED — WS2812

| Arduino Pin | LED |
|-------------|-----|
| 4 | Data in |

Single NeoPixel. Brightness init to 20/255. Multiple LEDs can be addressed (0=back, 1=right, 2=front, 3=left, 4=middle) but only one physical LED on V4.0.

#### IR Receiver

| Arduino Pin |
|-------------|
| 9 |

NEC protocol. Two keymaps (remote type A and B) with known codes. Used for: direction control, mode switching, speed adjustment, tracking threshold tuning.

#### Voltage Divider

| Arduino Pin |
|-------------|
| A3 |

Reads `analogRead(A3) * 0.0375` with 8% compensation. Low battery threshold = 7.0V. Triggers LED warning pattern when low.

#### Mode Button

| Arduino Pin | Interrupt |
|-------------|-----------|
| 2 | INT0 (FALLING) |

Cycles value 0→1→2→3→0. Debounced to 500ms. Maps to modes: 1=Tracking, 2=Obstacle Avoidance, 3=Following, 0=Standby.

---

## 3. Command Protocol (Arduino Side)

All commands are **JSON frames delimited by `{...}`** over UART at 9600 baud. The protocol is documented in `Communication protocol for Smart Robot Car.pdf` in the `04 Related chip information/` folder.

### 3.1 Command structure

```json
{"N":<command_id>, "H":"<sequence>", "D1":<val>, ...}
```

Acknowledgment: `{<sequence>_ok}` or `{"ok"}`

### 3.2 Command reference

| N | Command | Params | Description |
|---|---------|--------|-------------|
| 1 | Motor single | D1=selection(0/1/2), D2=speed, D3=direction(1/2) | Control individual motor (0=both, 1=left, 2=right). Direction: 1=fwd, 2=back. No time limit. |
| 2 | Car move timed | D1=direction, D2=speed, T=ms | Move car with time limit. Direction: 1=left, 2=right, 3=fwd, 4=back. Returns to programming mode after T expires. |
| 3 | Car move untimed | D1=direction, D2=speed | Move car indefinitely. Same direction encoding as N2. |
| 4 | Motor speeds | D1=left_speed, D2=right_speed | Set individual motor speeds (both forward). 0/0 = stop. |
| 5 | Servo | D1=servo(1/2/3), D2=angle(10x) | Move servo. 1=Z, 2=Y, 3=both. Angle is 10x degrees (e.g., 900 = 90°). Single shot, returns to programming mode. |
| 7 | LED timed | D1=pos, D2=R, D3=G, D4=B, T=ms | Set RGB with time limit. Position: 0=all, 1-5 for specific. |
| 8 | LED untimed | D1=pos, D2=R, D3=G, D4=B | Set RGB indefinitely. |
| 21 | Ultrasonic query | D1=mode(1/2) | 1=obstacle detection(true/false), 2=distance in cm |
| 22 | IR tracker query | D1=sensor(0/1/2) | 0=left, 1=middle, 2=right. Returns analog value. |
| 23 | Ground check | none | Returns whether car is lifted (true=off ground) |
| 100 | Stop + Standby | none | Clear all functions, stop motors, enter standby |
| 101 | Mode switch | D1=mode(1/2/3) | 1=tracking, 2=obstacle, 3=follow |
| 102 | Rocker control | D1=direction(1-5) | Direct joystick control. 1=fwd, 2=back, 3=left, 4=right, 5=stop |
| 105 | LED brightness | D1=up(1)/down(2) | Step brightness ±5 (range 0-255) |
| 106 | Servo step | D1=action(1-5) | 1=Y-up, 2=Y-down, 3=Z-up, 4=Z-down, 5=center-both |
| 110 | Stop + Programming | none | Clear all functions, enter programming mode |

### 3.3 Mode enumeration (standalone modes, no serial needed)

- **Standby** — idle, purple breathing LED
- **Line Tracking** (mode 1) — follows black line using 3 IR sensors, green LED
- **Obstacle Avoidance** (mode 2) — ultrasonic scanning, servo pan, yellow LED
- **Following** (mode 3) — servo scanning for object within 20cm, then follows, blue LED
- **Rocker** — IR remote or app joystick manual control, violet LED

Mode switching: hardware button (pin 2), IR remote (buttons 6/7/8), or serial command N101.

---

## 4. Motor Control Logic (extracted)

The motor driver is a **TB6612FNG** dual H-bridge. Two motors: A=right, B=left.

```cpp
// Low-level motor control
void motor_control(bool dirA, uint8_t speedA, bool dirB, uint8_t speedB, bool enable)
{
    if (!enable) { digitalWrite(STBY, LOW); return; }
    digitalWrite(STBY, HIGH);

    // Motor A (right)
    if (dirA == FORWARD)      { digitalWrite(AIN1, HIGH); analogWrite(PWMA, speedA); }
    else if (dirA == BACKWARD) { digitalWrite(AIN1, LOW);  analogWrite(PWMA, speedA); }
    else                       { analogWrite(PWMA, 0); digitalWrite(STBY, LOW); }

    // Motor B (left)
    if (dirB == FORWARD)      { digitalWrite(BIN1, HIGH); analogWrite(PWMB, speedB); }
    else if (dirB == BACKWARD) { digitalWrite(BIN1, LOW);  analogWrite(PWMB, speedB); }
    else                       { analogWrite(PWMB, 0); digitalWrite(STBY, LOW); }
}
```

**Yaw correction**: When driving straight (forward/back), the MPU6050 Z-gyro is used for PID-like proportional correction. The yaw error `(Yaw - yaw_So) * Kp` adjusts left/right motor speeds asymmetrically to keep heading straight. Kp varies by mode (2-10).

### Direction mappings for high-level commands

| Command | Motor A (right) | Motor B (left) |
|---------|-----------------|-----------------|
| Forward | fwd, speed* | fwd, speed* |
| Backward | back, speed* | back, speed* |
| Left | fwd, speed | back, speed |
| Right | back, speed | fwd, speed |
| LeftForward | fwd, speed | fwd, speed/2 |
| LeftBackward | back, speed | back, speed/2 |
| RightForward | fwd, speed/2 | fwd, speed |
| RightBackward | back, speed/2 | back, speed |
| Stop | coast, 0 | coast, 0 |

*Forward/backward use yaw-corrected asymmetric speeds in all modes except line tracking.

---

## 5. Connection Diagram

```
                    PHYSICAL CONNECTIONS
                    ====================

ESP32-CAM                          Arduino UNO
─────────                          ───────────
GPIO33 (WROVER) or GPIO3 (S3) ────► TX (pin 1)
GPIO4 (WROVER) or GPIO40 (S3) ◄─── RX (pin 0)
GND ─────────────────────────────── GND

Arduino UNO                        Peripherals
───────────                        ───────────
Pin 5  ──── TB6612 PWMA    (right motor PWM)
Pin 6  ──── TB6612 PWMB    (left motor PWM)
Pin 7  ──── TB6612 AIN1    (right motor dir)
Pin 8  ──── TB6612 BIN1    (left motor dir)
Pin 3  ──── TB6612 STBY    (motor enable)
Pin 13 ──── HC-SR04 TRIG   (ultrasonic trigger)
Pin 12 ──── HC-SR04 ECHO   (ultrasonic echo)
Pin A2 ──── IR sensor L    (left)
Pin A1 ──── IR sensor M    (middle)
Pin A0 ──── IR sensor R    (right)
Pin A4 ──── MPU6050 SDA    (I2C)
Pin A5 ──── MPU6050 SCL    (I2C)
Pin 10 ──── Servo Z        (pan)
Pin 11 ──── Servo Y        (tilt)
Pin 4  ──── WS2812 DIN     (RGB LED)
Pin 9  ──── IR Receiver    (NEC remote)
Pin A3 ──── Voltage divider
Pin 2  ──── Mode button    (INT0, active LOW with pullup)
```

---

## 6. Key Libraries Used

### Arduino side
- **FastLED** — WS2812 control
- **MPU6050** + **I2Cdev** — IMU (Jeff Rowberg library)
- **IRremote** — NEC infrared decoding
- **ArduinoJson v6** — JSON serial protocol parsing
- **Servo** — standard Arduino servo library

### ESP32 side
- **esp_camera** — OV2640 camera driver
- **esp_http_server** — HTTP server for camera streaming
- **WiFi** — AP mode networking
- **img_converters** — JPEG/RGB conversion
- **fd_forward / fr_forward** — face detection/recognition (ESP-WHO)

---

## 7. RPi Integration Notes

If replacing the phone app with an RPi:

1. **Connect RPi to ESP32 WiFi** (`ELEGOO-XXXXXX`, no password)
2. **Open TCP socket** to `192.168.4.1:100`
3. **Send JSON commands** as documented in section 3
4. **Handle heartbeat**: respond to `{Heartbeat}` within 3 seconds
5. **Stream video** from `http://192.168.4.1:81/stream` (MJPEG)

Alternatively, bypass the ESP32 entirely and connect the RPi's UART directly to Arduino pins 0/1 at 9600 baud. The protocol is the same JSON frames. This would eliminate the WiFi hop and let the RPi serve as both controller and web server.

The RPi could also directly control the TB6612 and sensors, but you lose the Arduino's tight real-time motor control loop unless you run a real-time kernel or use the RPi's hardware PWM properly.
