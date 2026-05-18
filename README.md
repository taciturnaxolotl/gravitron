# K2-B4

A gesture-controlled robot built on the ELEGOO Smart Robot Car V4.0. You drive it with your hands in front of a webcam and see through its eyes via a phone strapped to the chassis. The whole thing runs on ESP-NOW for low-latency radio, WebRTC for the camera feed, and MediaPipe for hand tracking.

<img width="1280" height="960" alt="IMG_9621 Large" src="https://github.com/user-attachments/assets/2f5daba7-e0ec-4028-a184-a25964ca4558" />
<img width="960" height="1280" alt="IMG_9625 Large" src="https://github.com/user-attachments/assets/f69f6471-1070-4c2f-a1f6-258c938398b7" />
<img width="960" height="1280" alt="IMG_9624 Large" src="https://github.com/user-attachments/assets/612724be-0580-40f5-badf-00fd5b9382f3" />
<img width="1280" height="960" alt="IMG_9623 Large" src="https://github.com/user-attachments/assets/a72a6477-d184-4190-9d74-b4edc4d30b8f" />
<img width="1280" height="960" alt="IMG_9622 Large" src="https://github.com/user-attachments/assets/a2a72bc9-c3fb-4793-8756-64e7cf22b5da" />


## How it works

```
Your laptop                      Phone on robot             Robot
┌─────────────┐    WebRTC       ┌──────────────┐
│ controller  │◄───video────────│  phone.html  │
│  .html      │                 └──────────────┘
│             │    USB serial   ┌──────────────┐  ESP-NOW   ┌──────────────┐  UART   ┌─────────┐
│ hand track  │────0xAB frame──►│ transmitter  │───radio───►│   receiver   │────────►│ Arduino │
│ (MediaPipe) │                 │   (ESP32)    │            │   (ESP32)    │         │  motors │
└─────────────┘                 └──────────────┘            └──────────────┘         └─────────┘
```

Two ESP32s talk over ESP-NOW. The transmitter sits plugged into your laptop via USB serial, the receiver sits on the robot wired to the Arduino UNO that actually drives the motors. A phone mounted on the robot streams its rear camera back to the controller page over WebRTC so you can see where you're going.

## Controls

**Hand mode** (default): hold both hands up in the camera's steer zone (right 80% of frame) and clench your fists to drive. Tilt the line between your hands to steer. Set throttle by clenching in the left 20% zone; hand height controls speed. Throttle persists when you release. Open either hand to stop.

**Gamepad mode**: left trigger drives the left motor, right trigger drives the right. B button reverses. The phone feed stays up in both modes.

**E-stop**: hit spacebar. Everything goes to zero immediately, screen goes red. Click the enable button in the top right to get back.

## Running it

You need Node for the signaling server and ngrok (or similar) so the phone can reach it over HTTPS.

```bash
cd server
npm install
npm start        # runs on :3000
ngrok http 3000  # separate terminal
```

Open the ngrok URL at `/controller` on your laptop. Scan the QR code with your phone (or go to `/phone.html`). The phone streams its camera back and the controller picks it up automatically if the phone is already connected.

Serial connect button is in the top left. Plug in the transmitter ESP32 and click it to pair.

## Binary frame protocol

Everything between the laptop and transmitter is a 5-byte binary frame at 921600 baud:

| Byte | Content |
|------|---------|
| 0 | `0xAB` sync |
| 1 | left speed (int8, -127 to 127) |
| 2 | right speed (int8, -127 to 127) |
| 3 | buttons (uint8 bitmask) |
| 4 | CRC8 of bytes 1-3 |

The transmitter unpacks this and fires it over ESP-NOW. The receiver sends back `0xAE` on success or `0xAD` on failure so the controller can show link quality.

## Project structure

```
├── esp32-firmware/
│   ├── controller.html        # the whole UI, hand tracking, WebRTC client
│   ├── transmitter/           # ESP32 that reads USB serial, sends ESP-NOW
│   └── receiver/              # ESP32 on robot, receives ESP-NOW, drives Arduino
├── server/
│   ├── server.js              # signaling server for WebRTC + serves pages
│   └── public/
│       └── phone.html         # phone camera capture page
├── arduino-firmware/
│   └── smartcar_firmware/     # stock ELEGOO firmware with JSON serial responses
├── flake.nix                  # nix dev shell
└── Makefile                   # build + flash commands
```

## Flashing

```bash
ls /dev/cu.usb*

# Arduino (USB-B)
make arduino-flash ARDUINO_PORT=/dev/cu.usbmodemXXXXX

# ESP32 transmitter/receiver (USB-C)
make esp32-flash ESP_PORT=/dev/cu.usbserial-XXXXX
```

## Dependencies

Hand tracking uses MediaPipe Tasks Vision loaded from CDN. QR code from `qrcode-generator`. No build step for the frontend, it's all one HTML file.

The signaling server is Express + ws. The phone and controller connect over WebSocket for signaling and then go peer-to-peer for the actual video stream.
