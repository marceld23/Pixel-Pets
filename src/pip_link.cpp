#include "pip_link.h"
#include "target_caps.h"

#if TARGET_HAS_WIFI

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <string.h>

#include "net.h"

// ─── Pip-link companion listener ────────────────────────────────────────
//
// Listens on the shared ESP-NOW broadcast channel for packets from a
// paired Pip and routes them by msgType into per-feature handlers. Reuses
// openEspNowRadio / closeEspNowRadio from net.cpp so it sits on the same
// LR PHY profile as Friends mode — the asymmetric-RF reliability fixes
// from Friends carry over.
//
// Lifecycle:
//   begin(true)  → openEspNowRadio + esp_now_init + register cb + add peer
//   end()        → unregister cb + del peer + deinit + closeEspNowRadio
//   pause()      → same teardown as end() but keeps g_enabled, so a later
//                  resume() restarts the listener. Used by friendsBegin
//                  and webradio::start (channel/radio conflicts).
//   resume()     → if enabled and currently paused, repeat the begin path.
//   tick(now)    → drain RX queue, decode 16-byte GOOG packets, dispatch.
//
// The RX queue is a single-producer / single-consumer ring with volatile
// head/tail (callback writes, main loop reads). Same pattern as Friends.
// Up to 8 in-flight packets — beyond that the callback drops the newest.

namespace {

// RX ring — written from the WiFi task (recv callback), read from the
// main task (pip_link::tick). Volatile head/tail keeps it lock-free under
// the single-producer / single-consumer assumption.
constexpr uint8_t kRxQueueSize = 8;
struct RxItem { uint8_t mac[6]; uint8_t data[32]; uint8_t len; };
RxItem            g_rx[kRxQueueSize];
volatile uint8_t  g_rxHead = 0;
volatile uint8_t  g_rxTail = 0;
volatile uint32_t g_rxCbCount = 0;   // diagnostics

// Run-state flags. g_enabled mirrors Persisted::pipMode; g_paused tracks
// whether another feature has temporarily claimed the radio. The radio
// is up iff g_enabled && !g_paused && g_radioReady.
bool g_enabled    = false;
bool g_paused     = false;
bool g_radioReady = false;     // ESP-NOW + peer actually set up

// Identity bytes — match Friends mode so a packet from this device is
// recognisable on the air.
uint8_t g_myId[4]      = {0};
bool    g_myIdLoaded   = false;

// Diagnostics.
uint32_t g_lastHeartbeatMs = 0;
uint32_t g_rxProcessedCount = 0;
uint32_t g_treatCount       = 0;

pip_link::TreatHandler g_treatHandler = nullptr;
pip_link::WandHandler  g_wandHandler  = nullptr;

// Same dedup pattern as Friends — drop duplicate eids from a 5-slot ring.
constexpr uint8_t kEidRingSize = 5;
uint32_t g_recentEids[kEidRingSize] = {0};
uint8_t  g_recentEidIdx = 0;

bool eidSeenOrRecord(uint32_t eid) {
    if (eid == 0) return false;
    for (uint8_t i = 0; i < kEidRingSize; ++i) {
        if (g_recentEids[i] == eid) return true;
    }
    g_recentEids[g_recentEidIdx] = eid;
    g_recentEidIdx = (uint8_t)((g_recentEidIdx + 1) % kEidRingSize);
    return false;
}

// Recv callback — runs in the WiFi task. Keep it short: copy data, bail.
void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len <= 0 || len > (int)sizeof(g_rx[0].data)) return;
    uint8_t next = (uint8_t)((g_rxHead + 1) % kRxQueueSize);
    if (next == g_rxTail) return;     // queue full — drop newest
    RxItem& item = g_rx[g_rxHead];
    memcpy(item.mac, mac, 6);
    memcpy(item.data, data, len);
    item.len = (uint8_t)len;
    g_rxHead = next;
    g_rxCbCount++;
}

void onSend(const uint8_t* /*mac*/, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        Serial.printf("[pip-link] tx send-cb status=%d (fail)\n", (int)status);
    }
}

void loadMyId() {
    if (g_myIdLoaded) return;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    g_myId[0] = mac[2];
    g_myId[1] = mac[3];
    g_myId[2] = mac[4];
    g_myId[3] = mac[5];
    g_myIdLoaded = true;
}

bool tryDecode(const uint8_t* data, size_t len, uint8_t* outType,
               uint8_t outId[4], uint8_t* outAnimal, uint32_t* outEid,
               uint8_t* outPayloadByte) {
    if (len < kEspNowPacketLen) return false;
    if (memcmp(data, kEspNowProtoMagic, 4) != 0) return false;
    if (data[4] != kEspNowProtoVersion) return false;
    *outType  = data[5];
    outId[0] = data[6]; outId[1] = data[7];
    outId[2] = data[8]; outId[3] = data[9];
    *outAnimal = data[10];
    *outEid = ((uint32_t)data[12] <<  0) |
              ((uint32_t)data[13] <<  8) |
              ((uint32_t)data[14] << 16) |
              ((uint32_t)data[15] << 24);
    *outPayloadByte = data[12];   // first byte of the eid range — also the
                                  // single-byte payload for kMsgPipTreat
                                  // (kind), kMsgPipWand (gesture id) etc.
    return true;
}

bool startRadio() {
    if (g_radioReady) return true;
    loadMyId();

    if (!openEspNowRadio("pip-link")) {
        Serial.println(F("[pip-link] openEspNowRadio failed"));
        return false;
    }

    if (esp_now_init() != ESP_OK) {
        Serial.println(F("[pip-link] esp_now_init failed"));
        closeEspNowRadio();
        return false;
    }
    esp_err_t recvCbRes = esp_now_register_recv_cb(onRecv);
    esp_err_t sendCbRes = esp_now_register_send_cb(onSend);
    Serial.printf("[pip-link] register_recv_cb=%d register_send_cb=%d\n",
                  (int)recvCbRes, (int)sendCbRes);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, kEspNowBroadcastMac, 6);
    peer.channel = kEspNowChannel;
    peer.encrypt = false;
    esp_err_t addRes = esp_now_add_peer(&peer);
    if (addRes != ESP_OK && addRes != ESP_ERR_ESPNOW_EXIST) {
        Serial.printf("[pip-link] esp_now_add_peer failed: %d\n", (int)addRes);
    }

    g_rxHead = g_rxTail = 0;
    g_lastHeartbeatMs = millis();
    memset(g_recentEids, 0, sizeof(g_recentEids));
    g_recentEidIdx = 0;
    g_radioReady = true;
    Serial.printf("[pip-link] listener ready, channel=%u\n",
                  (unsigned)kEspNowChannel);
    return true;
}

void stopRadio() {
    if (!g_radioReady) return;
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_del_peer(kEspNowBroadcastMac);
    esp_now_deinit();
    closeEspNowRadio();
    g_radioReady = false;
    Serial.println(F("[pip-link] listener stopped"));
}

}  // namespace

namespace pip_link {

void begin(bool enabled) {
    g_enabled = enabled;
    g_paused  = false;
    if (enabled) startRadio();
}

void end() {
    g_enabled = false;
    g_paused  = false;
    stopRadio();
}

bool isActive() { return g_enabled && !g_paused && g_radioReady; }

void pause() {
    if (!g_enabled) return;
    g_paused = true;
    stopRadio();
}

void resume() {
    if (!g_enabled) return;
    if (!g_paused) return;
    g_paused = false;
    startRadio();
}

void setTreatHandler(TreatHandler fn) { g_treatHandler = fn; }
void setWandHandler (WandHandler  fn) { g_wandHandler  = fn; }

void tick(uint32_t now_ms) {
    if (!isActive()) return;

    // Heartbeat every 5 s — same diagnostics format as Friends so log
    // diffs are easy to read side-by-side.
    if (now_ms - g_lastHeartbeatMs >= 5000) {
        g_lastHeartbeatMs = now_ms;
        Serial.printf(
          "[pip-link] heartbeat: rxCb=%lu rx=%lu treats=%lu\n",
          (unsigned long)g_rxCbCount,
          (unsigned long)g_rxProcessedCount,
          (unsigned long)g_treatCount);
    }

    // Drain the queue. One Edit per packet keeps the recv-cb side cheap.
    while (g_rxTail != g_rxHead) {
        RxItem& item = g_rx[g_rxTail];
        g_rxTail = (uint8_t)((g_rxTail + 1) % kRxQueueSize);
        g_rxProcessedCount++;

        uint8_t  type, id[4], animal, payloadByte;
        uint32_t eid = 0;
        if (!tryDecode(item.data, item.len, &type, id, &animal, &eid,
                       &payloadByte)) {
            continue;
        }
        if (memcmp(id, g_myId, 4) == 0) continue;     // own packet
        // Ignore Friends-protocol msgTypes (0..7). Friends mode is paused
        // while we're listening, so we shouldn't see these — but if a
        // stray packet from another nearby project arrives, drop it.
        if (type < kMsgPipTreat || type > kMsgPipShutter) continue;
        if (eidSeenOrRecord(eid)) continue;           // burst dedup

        switch (type) {
            case kMsgPipTreat: {
                PipTreatKind kind = (PipTreatKind)payloadByte;
                if (kind > PipTreatBone) kind = PipTreatApple;
                Serial.printf(
                  "[pip-link] treat received kind=%u animal=%u eid=%lu\n",
                  (unsigned)kind, (unsigned)animal, (unsigned long)eid);
                g_treatCount++;
                if (g_treatHandler) g_treatHandler(kind, animal, now_ms);
                break;
            }
            case kMsgPipWand: {
                uint8_t peakCount = payloadByte;
                Serial.printf(
                  "[pip-link] wand received peaks=%u animal=%u eid=%lu\n",
                  (unsigned)peakCount, (unsigned)animal, (unsigned long)eid);
                if (g_wandHandler) g_wandHandler(peakCount, animal, now_ms);
                break;
            }
            // Other msgTypes (17 step report, 19 egg, 20 shutter) are
            // reserved but not on the active roadmap; log + drop.
            default:
                Serial.printf(
                  "[pip-link] unhandled msgType=%u\n", (unsigned)type);
                break;
        }
    }
}

}  // namespace pip_link

#else   // !TARGET_HAS_WIFI — Pip target. No listener, header still
        // includes safely; functions compile to no-ops.

namespace pip_link {
void begin(bool)                   {}
void end()                         {}
bool isActive()                    { return false; }
void pause()                       {}
void resume()                      {}
void setTreatHandler(TreatHandler) {}
void setWandHandler (WandHandler)  {}
void tick(uint32_t)                {}
}  // namespace pip_link

#endif  // TARGET_HAS_WIFI
