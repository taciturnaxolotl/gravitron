#!/usr/bin/env python3
"""
Test motor firmware directly over serial.
Sends 5-byte frames (0xAB | left | right | buttons | CRC8) to Arduino.
Expects 0xAC ACK byte per valid frame.
"""
import sys
import time

import serial


def crc8(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def make_frame(left: int, right: int, buttons: int = 0) -> bytes:
    payload = bytes([left & 0xFF, right & 0xFF, buttons])
    return bytes([0xAB]) + payload + bytes([crc8(payload)])


def send_and_ack(ser, frame: bytes, label: str) -> bool:
    ser.write(frame)
    ack = ser.read(1)
    ok = ack == b"\xAC"
    print(f"  {label}: {'ACK' if ok else 'NO ACK' if ack else 'timeout'}")
    return ok


PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-110"
ser = serial.Serial(PORT, 115200, timeout=0.3)
time.sleep(2)  # wait for Arduino reset

print(f"Connected to {PORT}\n")

# === Test cases ===
tests = [
    ("forward 50%",  make_frame(64, 64)),
    ("forward 100%", make_frame(127, 127)),
    ("reverse 50%",  make_frame(-64, -64)),
    ("reverse 100%", make_frame(-128, -128)),
    ("spin left",    make_frame(-64, 64)),
    ("spin right",   make_frame(64, -64)),
    ("brake",        make_frame(0, 0)),
]

results = []
for label, frame in tests:
    ok = send_and_ack(ser, frame, label)
    results.append((label, ok))
    time.sleep(0.15)

print(f"\nResults: {sum(r[1] for r in results)}/{len(results)} ACK received")
for label, ok in results:
    if not ok:
        print(f"  FAIL: {label}")

ser.close()
