#include "pip_link_send.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <string.h>

#include "../net.h"          // kEspNowChannel, kEspNowBroadcastMac, magic, etc.
#include "../pip_link.h"      // kMsgPipTreat — protocol value

// ─── Pip-side single-shot sender ───────────────────────────────────────────
//
// Pip can't afford to keep the radio on continuously (200 mAh battery, 80+
// mA radio cost = a few hours instead of all-day standby). Every call to
// sendTreat does the full lifecycle: power up WiFi + ESP-NOW → 3-burst
// transmit → power down. Total radio time per call ≈ 150-200 ms.
//
// Receiver expects the standard 802.11 B/G/N PHY profile (matches the
// bigger pet's openEspNowRadio() in net.cpp). LR-mode interop turned
// out to be unreliable across ESP32 ↔ ESP32-S3 — the receiver was
// switched to BGN, so the Pip sender has to match. We don't reuse
// net.cpp's openEspNowRadio helper here because Pip wants the absolute
// minimum init-time and skips the diagnostic readback prints.

namespace {

bool g_idLoaded = false;
uint8_t g_myId[4] = {0};

void loadMyId() {
    if (g_idLoaded) return;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    g_myId[0] = mac[2];
    g_myId[1] = mac[3];
    g_myId[2] = mac[4];
    g_myId[3] = mac[5];
    g_idLoaded = true;
}

bool radioUp() {
    // AP_STA mirrors openEspNowRadio() on the receiver side. On the
    // ESP32-S3 receiver, pure WIFI_STA without an AP association silently
    // drops most ESP-NOW broadcast frames; AP_STA gives the stack an AP
    // MAC table to dispatch broadcasts against. The AP interface stays
    // unused on the Pip sender side, but matching modes simplifies the
    // RF symmetry.
    WiFi.mode(WIFI_AP_STA);
    delay(20);
    esp_wifi_set_max_tx_power(84);
    esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    esp_wifi_set_channel(kEspNowChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_ps(WIFI_PS_NONE);   // last, so prior calls can't reset it

    if (esp_now_init() != ESP_OK) {
        Serial.println(F("[pip-link-tx] esp_now_init failed"));
        return false;
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, kEspNowBroadcastMac, 6);
    peer.channel = kEspNowChannel;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    return true;
}

void radioDown() {
    esp_now_del_peer(kEspNowBroadcastMac);
    esp_now_deinit();
    esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    WiFi.mode(WIFI_OFF);
}

void buildAndSend(uint8_t msgType, uint8_t payloadByte, uint8_t animal,
                  uint32_t eid) {
    uint8_t buf[kEspNowPacketLen] = {0};
    memcpy(buf, kEspNowProtoMagic, 4);
    buf[4]  = kEspNowProtoVersion;
    buf[5]  = msgType;
    buf[6]  = g_myId[0];
    buf[7]  = g_myId[1];
    buf[8]  = g_myId[2];
    buf[9]  = g_myId[3];
    buf[10] = animal;
    buf[11] = 0;             // language unused on Pip
    buf[12] = payloadByte;   // first byte of the eid range doubles as
                              // single-byte payload (treat kind / wand id)
    buf[13] = (uint8_t)(eid >>  8);
    buf[14] = (uint8_t)(eid >> 16);
    buf[15] = (uint8_t)(eid >> 24);
    esp_err_t r = esp_now_send(kEspNowBroadcastMac, buf, kEspNowPacketLen);
    Serial.printf("[pip-link-tx] msgType=%u payload=%u eid=%lu result=%d\n",
                  (unsigned)msgType, (unsigned)payloadByte,
                  (unsigned long)eid, (int)r);
}

}  // namespace

namespace pip {
namespace link {

void sendTreat(uint8_t kind, uint8_t animal) {
    loadMyId();
    if (!radioUp()) return;

    // Burst x3 with the same eid. Receiver dedups via the 5-slot eid ring.
    // The eid uses millis() — receiver's eid layout puts the kind in
    // byte 12, so we deliberately put the kind in byte 12 (overwriting
    // the low byte of the 32-bit eid field). The remaining 24 bits give
    // ~4.6 hours of unique-eid range, plenty for a single Pip session.
    uint32_t eid = millis();
    if (eid == 0) eid = 1;
    for (int i = 0; i < 3; ++i) {
        buildAndSend(pip_link::kMsgPipTreat, kind, animal, eid);
        if (i < 2) delay(20);
    }

    // Give the WiFi task a moment to flush the last TX before we tear
    // down — otherwise the final packet can be dropped on the floor.
    delay(20);
    radioDown();
}

void sendWand(uint8_t peakCount, uint8_t animal) {
    loadMyId();
    if (!radioUp()) return;

    uint32_t eid = millis();
    if (eid == 0) eid = 1;
    for (int i = 0; i < 3; ++i) {
        buildAndSend(pip_link::kMsgPipWand, peakCount, animal, eid);
        if (i < 2) delay(20);
    }
    delay(20);
    radioDown();
}

}  // namespace link
}  // namespace pip
