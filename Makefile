.PHONY: all esp32-build esp32-flash esp32-monitor esp32-menuconfig esp32-clean \
        arduino-init arduino-build arduino-flash arduino-monitor

# ── ESP32 ──  uses nix develop .#esp32
# ── Arduino ── uses nix develop .#arduino (or default)

VARIANT ?= 0
ESP_PORT ?= /dev/ttyUSB0
ARDUINO_PORT ?= /dev/cu.usbmodem*
ARDUINO_FQBN  = arduino:avr:uno

all: esp32-build

# ── ESP32 ──────────────────────────────────────────────────────────

esp32-build:
	nix develop .#esp32 --command idf.py -DROBOT_VARIANT=$(VARIANT) build

esp32-flash:
	nix develop .#esp32 --command idf.py -DROBOT_VARIANT=$(VARIANT) -p $(ESP_PORT) flash

esp32-run: esp32-build esp32-flash

esp32-monitor:
	nix develop .#esp32 --command idf.py -p $(ESP_PORT) monitor

esp32-menuconfig:
	nix develop .#esp32 --command idf.py menuconfig

esp32-clean:
	nix develop .#esp32 --command idf.py fullclean

# ── Arduino ────────────────────────────────────────────────────────

arduino-init:
	nix develop --command arduino-cli core install arduino:avr

arduino-build: arduino-init
	nix develop --command arduino-cli compile \
		--fqbn $(ARDUINO_FQBN) \
		--output-dir build/arduino \
		arduino-firmware/smartcar_firmware/smartcar_firmware.ino

arduino-flash: arduino-build
	nix develop --command arduino-cli upload \
		--fqbn $(ARDUINO_FQBN) \
		--port $(ARDUINO_PORT) \
		--input-dir build/arduino \
		arduino-firmware/smartcar_firmware/smartcar_firmware.ino

arduino-monitor:
	nix develop --command arduino-cli monitor \
		--port $(ARDUINO_PORT) \
		--config baudrate=9600
