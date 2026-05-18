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

static volatile uint8_t ack_target[6];

static volatile uint8_t pending_frame[5];
static volatile bool frame_ready = false;

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

    // Buffer frame for loop(); Serial.write() not safe in ESP-NOW callback
    uint8_t tmp[5];
    tmp[0] = 0xAB;
    memcpy(&tmp[1], data, sizeof(DrivePacket));
    tmp[4] = crc8(data, sizeof(DrivePacket));
    memcpy((void*)pending_frame, tmp, 5);
    memcpy((void*)ack_target, smac, 6);
    frame_ready = true;
}

#define RXD2 3
#define TXD2 40

void setup() {
    Serial.begin(115200);
    Serial1.begin(9600, SERIAL_8N1, RXD2, TXD2);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_54M);

    esp_now_init();
    esp_now_register_recv_cb(onRecv);
}

void loop() {
    // Flush pending serial frame (deferred from ESP-NOW callback)
    if (frame_ready) {
        uint8_t out[5];
        memcpy(out, (const void*)pending_frame, 5);
        frame_ready = false;
        Serial1.write(out, 5);
        Serial1.flush();

        // Wait up to 20ms for Arduino ACK
        unsigned long deadline = millis() + 20;
        while (millis() < deadline) {
            if (Serial1.available()) {
                if (Serial1.read() == 0xAC) {
                    uint8_t ack = 0xAC;
                    uint8_t mac[6];
                    memcpy(mac, (const void*)ack_target, 6);
                    esp_now_send(mac, &ack, 1);
                }
                break;
            }
        }
        // Drain anything leftover
        while (Serial1.available()) Serial1.read();
    }
}
