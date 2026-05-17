#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

struct __attribute__((packed)) DrivePacket {
    int8_t left;
    int8_t right;
    uint8_t buttons;
};

// E0:72:A1:6F:FC:FC
static uint8_t peer_mac[] = {0xE0, 0x72, 0xA1, 0x6F, 0xFC, 0xFC};

uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}

void onSend(const wifi_tx_info_t* tx_info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        Serial.write((uint8_t)0xAE);
    } else {
        Serial.write((uint8_t)0xAD);
    }
}

void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len == 1 && data[0] == 0xAC) {
        Serial.write((uint8_t)0xAC);  // full round trip: Arduino ack made it back
    }
}

enum State { WAIT_MAGIC, READ_PAYLOAD };
static State state = WAIT_MAGIC;
static uint8_t buf[sizeof(DrivePacket) + 1];
static uint8_t buf_idx = 0;

void setup() {
    Serial.begin(921600);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    esp_now_init();
    esp_now_register_send_cb(onSend);
    esp_now_register_recv_cb(onRecv);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, peer_mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

void loop() {
    while (Serial.available()) {
        uint8_t b = Serial.read();

        switch (state) {
            case WAIT_MAGIC:
                if (b == 0xAB) {
                    state = READ_PAYLOAD;
                    buf_idx = 0;
                }
                break;
            case READ_PAYLOAD:
                buf[buf_idx++] = b;
                if (buf_idx == sizeof(buf)) {
                    uint8_t expected = crc8(buf, sizeof(DrivePacket));
                    if (buf[sizeof(DrivePacket)] == expected) {
                        DrivePacket pkt;
                        memcpy(&pkt, buf, sizeof(DrivePacket));
                        esp_now_send(peer_mac, (uint8_t*)&pkt, sizeof(pkt));
                    }
                    state = WAIT_MAGIC;
                }
                break;
        }
    }
}
