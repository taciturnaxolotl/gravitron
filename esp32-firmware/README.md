# Robot Board — ESP32-CAM Firmware

Replaces the stock ELEGOO ESP32 firmware with a **WebSocket API** + **HTTP camera server**. The car is controlled over WiFi from any browser or WebSocket client.

## Quick Start

```bash
# Set up ESP-IDF (one time)
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh && . ./export.sh

# Build and flash
cd esp32-firmware
idf.py set-target esp32          # for WROVER (V1)
idf.py -DROBOT_VARIANT=0 build   # or =1 for S3 (V2)
idf.py -p /dev/ttyUSB0 flash monitor
```

## Variants

| Flag | Board | UART Pins | LED GPIO |
|------|-------|-----------|----------|
| `ROBOT_VARIANT=0` | ESP32-WROVER (V1) | GPIO33 (RX), GPIO4 (TX) | GPIO13 |
| `ROBOT_VARIANT=1` | ESP32-S3 (V2) | GPIO3 (RX), GPIO40 (TX) | GPIO46 |

Set via: `idf.py build -DROBOT_VARIANT=0` or edit the `#define` in `main.c`.

## Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Redirects to `/index.html` |
| `/index.html` | GET | Web control panel |
| `/stream` | GET | MJPEG camera stream |
| `/capture` | GET | Single JPEG frame |
| `/api/info` | GET | Camera + WiFi + endpoint info (JSON) |
| `/api/control?var=X&val=Y` | POST | Adjust camera settings |
| `/ws` | WS | WebSocket control API |

## WebSocket API

Send JSON frames. Each frame has a `"cmd"` field. The firmware translates commands to the Arduino JSON protocol over UART.

### Commands

**Move**
```json
{"cmd":"move", "dir":"forward", "speed":200}
{"cmd":"move", "dir":"forward", "speed":200, "time_ms":2000}
```
Direction: `forward`, `backward`, `left`, `right`. Speed: 60-255. Optional `time_ms` limits duration (0 = indefinite).

**Motor speeds**
```json
{"cmd":"motors", "left":200, "right":200}
```

**Stop**
```json
{"cmd":"stop"}
```

**Servo**
```json
{"cmd":"servo", "servo":"pan", "angle":90}
{"cmd":"servo_step", "action":1}
```
Actions: 1=tilt-up, 2=tilt-down, 3=pan-right, 4=pan-left, 5=center-both.

**LED**
```json
{"cmd":"led", "r":255, "g":0, "b":0}
{"cmd":"led", "r":255, "g":0, "b":0, "time_ms":3000}
{"cmd":"led_brightness", "direction":"up"}
```

**Mode**
```json
{"cmd":"mode", "mode":"tracking"}
```
Modes: `tracking`, `obstacle`, `follow`, `standby`.

**Sensors**
```json
{"cmd":"distance"}
{"cmd":"obstacle"}
{"cmd":"ground"}
{"cmd":"ir", "sensor":0}
```

**Rocker** (joystick direct control)
```json
{"cmd":"rocker", "direction":"forward"}
```
Direction: `forward`, `backward`, `left`, `right`.

**Camera info**
```json
{"cmd":"camera_info"}
```

### Heartbeat

Clients must send `heartbeat` every 3 seconds. The server responds with `heartbeat_ok`. After 3 missed heartbeats, all motors stop and clients are disconnected.

### Arduino Responses

All responses from the Arduino JSON protocol are broadcast to all WebSocket clients:
```json
{"type":"arduino","data":"{\"ok\"}"}
```

## Architecture

```
WebSocket Client → /ws → ESP32 → UART (9600 baud) → Arduino UNO
Browser          → /stream, /capture, /index.html (HTTP)
```

The ESP32 is purely a wireless gateway — it translates WebSocket JSON commands into the existing Arduino JSON protocol and bridges sensor responses back.

## Wiring (no changes needed)

The firmware uses the existing UART connection between ESP32 and Arduino. No rewiring required. The phone app can be replaced entirely by any WebSocket or HTTP client connected to the ESP32's WiFi.
