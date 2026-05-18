#!/usr/bin/env python3
"""
Debug: check if Arduino is alive and responding to frames.
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


PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-110"
ser = serial.Serial(PORT, 115200, timeout=0.5)
print(f"Opened {PORT}, waiting 3s for reset...")
time.sleep(3)

# Drain any boot noise
time.sleep(0.5)
garbage = b""
while ser.in_waiting:
    garbage += ser.read(ser.in_waiting)
if garbage:
    print(f"Boot garbage ({len(garbage)} bytes): {garbage.hex()}")

# Send a single frame
frame = make_frame(50, 50)
print(f"Sending: {frame.hex()}")
ser.write(frame)

# Wait and read everything
time.sleep(0.2)
response = b""
while ser.in_waiting:
    response += ser.read(ser.in_waiting)

if response:
    print(f"Response ({len(response)} bytes): {response.hex()}")
    for b in response:
        print(f"  byte: 0x{b:02X} ({b})")
else:
    print("No response at all")

# Try a few more
for i in range(3):
    ser.write(frame)
    time.sleep(0.1)
    r = ser.read(5)
    print(f"[{i}] read {len(r)} bytes: {r.hex() if r else 'empty'}")

ser.close()
