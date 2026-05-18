/*
 * Minimal serial loopback test for receiver ESP32 -> Arduino.
 * Sends a hardcoded drive frame every 500ms over Serial.
 * No WiFi, no ESP-NOW — just proves the UART link works.
 */
#include <Arduino.h>

#define RXD2 3
#define TXD2 40

void setup() {
    Serial.begin(115200);
    Serial1.begin(9600, SERIAL_8N1, RXD2, TXD2);
}

void loop() {
    // left=50, right=50, buttons=0
    uint8_t b = 0xAB;
    Serial1.write(b);
    Serial1.flush();

    delay(500);
}
