#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

struct __attribute__((packed)) DrivePacket {
    int8_t left;
    int8_t right;
    uint8_t buttons;
};

static uint8_t sender_mac[6];
static bool have_peer = false;

static bool waiting_ack = false;
static uint8_t ack_target[6];
static unsigned long ack_deadline = 0;

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
    Serial1.write(frame, sizeof(frame));

    // Defer ACK to loop() — never block in ESP-NOW callback
    memcpy(ack_target, smac, 6);
    ack_deadline = millis() + 20;
    waiting_ack = true;
}

void setup() {
    Serial1.begin(115200, SERIAL_8N1, 4, 3);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_54M);

    esp_now_init();
    esp_now_register_recv_cb(onRecv);
}

void loop() {
    if (waiting_ack && millis() < ack_deadline && Serial1.available() >= 1) {
        if (Serial1.read() == 0xAC) {
            uint8_t ack = 0xAC;
            esp_now_send(ack_target, &ack, 1);
        }
        waiting_ack = false;
    } else if (waiting_ack && millis() >= ack_deadline) {
        waiting_ack = false;
    }
}
