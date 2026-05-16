#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

struct __attribute__((packed)) DrivePacket {
    int8_t left;
    int8_t right;
    uint8_t buttons;
};

static uint8_t sender_mac[6];
static bool have_peer = false;

uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}

void addPeer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len != sizeof(DrivePacket)) return;

    uint8_t* smac = info->src_addr;
    if (!have_peer) {
        memcpy(sender_mac, smac, 6);
        addPeer(smac);
        have_peer = true;
    }

    DrivePacket pkt;
    memcpy(&pkt, data, sizeof(pkt));

    // Forward to Arduino over serial
    uint8_t frame[5];
    frame[0] = 0xAB;
    memcpy(&frame[1], data, sizeof(DrivePacket));
    frame[4] = crc8(data, sizeof(DrivePacket));
    Serial.write(frame, sizeof(frame));

    // Read Arduino's ACK (non-blocking, with short timeout)
    unsigned long start = millis();
    while (Serial.available() < 1 && millis() - start < 5) {}
    if (Serial.available() >= 1 && Serial.read() == 0xAC) {
        uint8_t ack = 0xAC;
        esp_now_send(smac, &ack, 1);
    }
}

void setup() {
    Serial.begin(115200);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    esp_now_init();
    esp_now_register_recv_cb(onRecv);
}

void loop() {}
