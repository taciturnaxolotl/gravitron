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


PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-110"
ser = serial.Serial(PORT, 115200, timeout=0.5)
time.sleep(2)  # wait for Uno reset

left, right, buttons = 50, 50, 0
payload = bytes([left & 0xFF, right & 0xFF, buttons])
frame = bytes([0xAB]) + payload + bytes([crc8(payload)])

print(f"Sending: {frame.hex()}  (crc=0x{crc8(payload):02x})")

for i in range(10):
    ser.write(frame)
    ack = ser.read(1)
    print(f"[{i}] ack: {ack.hex() if ack else 'timeout'}")
    time.sleep(0.1)
