# Robot Board

Firmware for the ELEGOO Smart Robot Car V4.0. Replaces the stock ESP32 firmware with a WebSocket API + HTTP camera server while keeping the Arduino UNO as the real-time motor/sensor controller.

## Architecture

```
ESP32-CAM (WiFi AP: ELEGOO-XXXXXX)
├── HTTP :80  → /stream, /capture, /api/info, /api/control, web UI
├── WS :81    → JSON commands → translated to text protocol → UART
└── UART Serial2 (9600 baud) → Arduino UNO
```

## Quick Start

### Prerequisites

- [Nix](https://nixos.org/download.html) with flakes enabled
- USB cable for the Arduino (USB-B) and the ESP32 shield (USB-C)
- `direnv allow` at the repo root (optional but convenient)

### One-time setup

```bash
# Install AVR core + Arduino libraries
nix develop --command arduino-cli core install arduino:avr
nix develop --command arduino-cli lib install "FastLED" "MPU6050" "IRremote" "Servo"
```

### Build & Flash

```bash
# Find your ports
ls /dev/cu.usb*

# Arduino (USB-B port on the UNO)
make arduino-flash ARDUINO_PORT=/dev/cu.usbmodemXXXXX

# ESP32 (USB-C port on the camera shield)
make esp32-flash ESP_PORT=/dev/cu.usbserial-XXXXX

# ESP32 serial monitor
make esp32-monitor ESP_PORT=/dev/cu.usbserial-XXXXX
```

The two boards have **separate USB ports** — they don't interfere. You can plug both in at once.

### Variants

| Variant | Board | UART Pins | LED GPIO | Make flag |
|---------|-------|-----------|----------|-----------|
| V1 (default) | ESP32-WROVER | GPIO33 (RX), GPIO4 (TX) | GPIO13 | `VARIANT=0` |
| V2 | ESP32-S3 | GPIO3 (RX), GPIO40 (TX) | GPIO46 | `VARIANT=1` |

```bash
make esp32-build VARIANT=0   # WROVER
make esp32-build VARIANT=1   # S3
```

## ESP32 Firmware (`esp32-firmware/`)

### Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Redirects to `/index.html` |
| `/index.html` | GET | Web control panel |
| `/stream` | GET | MJPEG camera stream |
| `/capture` | GET | Single JPEG frame |
| `/api/info` | GET | Camera + WiFi info (JSON) |
| `/api/control?var=X&val=Y` | POST | Adjust camera settings |
| `/ws` | WS | WebSocket control API |

### WebSocket API

Connect to `ws://192.168.4.1/ws`. Send JSON frames with a `"cmd"` field. The ESP32 translates commands to the Arduino's text protocol over UART.

**Heartbeat:** Send `heartbeat` every 3 seconds. The server responds with `heartbeat_ok`. After 3 missed heartbeats, motors stop and all clients disconnect.

#### Commands

**Move**
```json
{"cmd":"move", "dir":"forward", "speed":200}
```
Direction: `forward`, `backward`, `left`, `right`. Speed: 60-255.

**Raw motor speeds**
```json
{"cmd":"motors", "left":200, "right":200}
```
Positive = forward, negative = backward.

**Stop**
```json
{"cmd":"stop"}
```

**Servo**
```json
{"cmd":"servo", "servo":"pan", "angle":90}
{"cmd":"servo_step", "action":1}
```
Actions: 1=tilt-up, 2=tilt-down, 3=pan-right, 4=pan-left.

**LED**
```json
{"cmd":"led", "r":255, "g":0, "b":0}
```

**Speed**
```json
{"cmd":"speed", "value":150}
```

**Sensors**
```json
{"cmd":"distance"}
{"cmd":"ir"}
{"cmd":"battery"}
{"cmd":"yaw"}
{"cmd":"calibrate"}
```

**Camera info**
```json
{"cmd":"camera_info"}
```

#### Responses

Arduino responses are wrapped and broadcast to all WebSocket clients:
```json
{"type":"arduino","data":{"ok":"forward"}}
{"type":"arduino","data":{"distance":42}}
{"type":"arduino","data":{"ir":{"l":123,"m":456,"r":789}}}
{"type":"arduino","data":{"yaw":-1.23}}
{"type":"arduino","data":{"battery":7.4}}
```

### Protocol (UART)

The ESP32 sends short text commands to the Arduino at 9600 baud:

| Command | Description |
|---------|-------------|
| `f` | Forward |
| `b` | Backward |
| `l` | Left |
| `r` | Right |
| `s` | Stop |
| `us` | Ultrasonic distance |
| `ir` | IR tracker all three |
| `bat` | Battery voltage |
| `yaw` | Read yaw angle |
| `cal` | Calibrate gyro |
| `led R G B` | Set RGB LED |
| `ledoff` | Turn LED off |
| `speed N` | Set move speed |
| `m dirA spdA dirB spdB` | Raw motor control |

The Arduino responds with compact JSON lines like `{"ok":"forward"}`, `{"distance":42}`, `{"yaw":-1.23}`.

## Arduino Firmware (`arduino-firmware/`)

The stock ELEGOO firmware with one change — serial responses use JSON instead of natural language so the ESP32 can forward them directly to WebSocket clients without parsing.

### Dependencies (via arduino-cli)

- FastLED
- MPU6050 (includes I2Cdev)
- IRremote
- Servo
- Wire (built-in)

## Wiring

No changes needed — uses the existing UART connection between the ESP32-CAM shield and Arduino UNO pins 0/1. The ESP32 shield sits on top of the Arduino normally.

## Project Structure

```
├── flake.nix               # Nix dev shells (arduino-cli)
├── Makefile                # Build commands
├── .envrc                  # direnv integration
├── HARDWARE_REFERENCE.md   # Original hardware docs
├── esp32-firmware/
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   ├── partitions.csv
│   ├── components/
│   │   └── esp_camera/     # Cloned from Espressif (v2.1.6)
│   ├── managed_components/ # Auto-resolved dependencies
│   └── main/
│       ├── main.c          # WiFi AP, init, LED
│       ├── camera.c/h      # OV2640 init
│       ├── uart_bridge.c/h # Serial2 at 9600
│       ├── websocket.c/h   # /ws handler, command translation
│       ├── http_server.c/h # /stream, /capture, /api/*
│       ├── cJSON.c/h       # Embedded JSON parser
│       └── index.html      # Web control panel
└── arduino-firmware/
    └── smartcar_firmware/
        └── smartcar_firmware.ino
```
