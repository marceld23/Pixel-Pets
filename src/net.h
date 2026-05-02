#pragma once
#include <Arduino.h>

// ─── Credential storage (separate "wifi" Preferences namespace) ─────────
//
// Up to kMaxWifiSlots known networks are stored. Boot-time connect picks
// the strongest visible one via WiFiMulti. The captive-portal "save"
// upserts: known SSID → password updated; new SSID → appended; list full
// → oldest entry dropped.

constexpr uint8_t kMaxWifiSlots = 5;

struct WifiCred {
  char ssid[33];
  char psk[65];
};

bool             wifiHasCreds();
uint8_t          wifiSlotsCount();
const WifiCred*  wifiSlot(uint8_t i);    // nullptr if i >= count
const char*      wifiCachedSsid();        // first slot's SSID or ""

bool wifiLoadCreds();           // refresh entire cache from NVS
void wifiAddCreds(const char* ssid, const char* psk);
void wifiClearCreds();          // wipes all slots

// ─── Boot-time STA connect + NTP time sync ──────────────────────────────
//
// Call wifiBeginAsync() right after persistence has been loaded — it kicks
// off WiFi.begin() in non-blocking mode so the radio can connect during
// the splash screen. wifiSyncTimeAndDisconnect() then waits briefly for
// the connection (if not already up), runs NTP, sets the RTC, and tears
// WiFi down again to save power. Returns true on successful time sync.

void wifiBeginAsync();
bool wifiSyncTimeAndDisconnect(uint32_t connectTimeoutMs = 6000,
                               uint32_t ntpTimeoutMs     = 4000);
// Same as wifiSyncTimeAndDisconnect, but leaves the radio on + connected
// so the caller can do extra HTTP work before tearing it down with
// wifiPowerOff(). Returns true on successful time sync.
bool wifiSyncTimeKeepConnected(uint32_t connectTimeoutMs = 6000,
                               uint32_t ntpTimeoutMs     = 4000);
// Granular split of the sync cycle — the boot task (see main.cpp) uses
// these to publish stages for the status UI.
// Call order: wifiConnect → wifiSyncTime → optionally fetchWorld... → wifiPowerOff.
bool wifiConnect (uint32_t connectTimeoutMs);   // STA + WiFiMulti.run + retry
bool wifiSyncTime(uint32_t ntpTimeoutMs);       // SNTP + HTTP-Date fallback
void wifiPowerOff();

// ─── Reference-counted WiFi keep-alive ──────────────────────────────────
//
// Several features want to keep WiFi open in parallel (web radio,
// Friends, parent server, captive portal). Rather than giving each
// feature its own on/off, each takes a reason string as a reference here.
// WiFi only goes off again once all references are released.
//
// reason is a static string (e.g. "webradio") and identifies the source
// for debug logs. Double take/release on the same reason are dedup'd
// (idempotent). Connect runs async; poll isReady().

bool wifiKeepAlive(const char* reason);   // true if a new reference, false if dedup
bool wifiRelease  (const char* reason);   // true if a reference was removed
bool wifiKeepAliveActive();                // true if >=1 reference is active
bool wifiIsConnected();                    // STA status

// ─── Captive-portal setup mode ──────────────────────────────────────────

enum class WifiSetupState : uint8_t {
  Idle,
  Active,        // AP "goo-goo-setup" + DNS + web server up, waiting for submit
  Submitted,     // POST received, briefly keeping web server alive for the response
  Connecting,    // AP torn down, STA connect attempt in flight
  Success,
  Failed,
};

void               wifiSetupBegin();
void               wifiSetupCancel();
void               wifiSetupTick();      // call every loop iteration while !Idle
WifiSetupState     wifiSetupGetState();
const char*        wifiSetupPendingSsid();   // ssid the user just submitted
uint8_t            wifiSetupScanCount();     // -1 → no scan yet, otherwise count

// ─── Shared ESP-NOW broadcast radio ─────────────────────────────────────
//
// Used by Friends mode (this file) and the Pip-link companion listener
// (pip_link.cpp). Both share the same PHY profile so the receiver-side
// reliability lessons from Friends carry over for free: Long-Range PHY,
// channel 6, max TX power, no power save.
//
// They cannot be active simultaneously — only one ESP-NOW recv-cb can be
// registered per stack — so callers coordinate via friends ↔
// pip_link::pause/resume.
//
// 16-byte broadcast packet layout (shared protocol):
//   [0..3]   magic "GOOG"
//   [4]      version (1)
//   [5]      msgType — Friends uses 0..7, Pip-link reserves 16..31
//   [6..9]   sender id (4 bytes of MAC)
//   [10]     animal (0=Bear, 1=Cat, 2=Dog)
//   [11]     language (0=DE, 1=EN)
//   [12..15] event-id — for receiver-side dedup of burst rebroadcasts

constexpr uint8_t kEspNowChannel        = 6;
constexpr uint8_t kEspNowProtoVersion   = 1;
constexpr uint8_t kEspNowPacketLen      = 16;
extern const uint8_t kEspNowBroadcastMac[6];
extern const uint8_t kEspNowProtoMagic[4];

// Open the radio for ESP-NOW broadcast on kEspNowChannel with LR PHY +
// max TX power + no power-save. Caller must follow with esp_now_init() +
// register cbs + add the broadcast peer. logPrefix is used in serial logs
// ("friends" / "pip-link").
bool openEspNowRadio(const char* logPrefix);

// Tear down: restore B/G/N protocol, WiFi off. Caller must already have
// removed the peer + unregistered cbs + esp_now_deinit'd.
void closeEspNowRadio();

// ─── Friends mode (UDP peer discovery) ───────────────────────────────────
//
// Activated through the Media → Friends menu. While running, the pet
// broadcasts a small UDP packet on the local LAN every second and listens
// for similar packets from other pets / the simulator. As soon as another
// pet is heard, both transition to the Matched state; the rendering layer
// plays a celebratory animation. WiFi is auto-connected when entering the
// mode and disconnected on exit.

// New flow: tap "Friends" → sending screen with 4 item buttons (Gift,
// Heart, Food, Game). Tap = ESP-NOW broadcast. After 5 sends or a 60 s
// timeout the screen closes and main.cpp plays a sequence animation of
// what was received. 30 s cooldown before the next session.

enum class FriendsState : uint8_t {
  Idle,
  Connecting,   // legacy — kept for view-rendering compatibility
  Searching,    // legacy alias — never used in the new flow
  Sending,      // 4-button screen, ESP-NOW running
  Matched,      // legacy alias — kept for backwards compat
  NoFriend,
  // Rendezvous: both pets must tap the big "Meet up" button at roughly
  // the same time. On match → Sending. Otherwise timeout.
  Rendezvous,
};

constexpr uint8_t kFriendsMaxRxItems = 5;
constexpr uint8_t kFriendsMaxSends   = 5;

// Item-type bytes — also used as the protocol's msg-type field on the
// wire so the receive side just reads `data[5]` and uses it directly.
enum FriendsItemKind : uint8_t {
  FriendsItemNone  = 0,
  FriendsItemGift  = 2,
  FriendsItemHeart = 3,
  FriendsItemFood  = 4,
  FriendsItemGame  = 5,
};

struct FriendsRxItem {
  uint8_t kind;          // FriendsItemKind
  uint8_t senderAnimal;  // 0 Bear, 1 Cat, 2 Dog
};

void           friendsBegin(uint8_t myAnimal, uint8_t myLanguage);
void           friendsEnd();
void           friendsTick(uint32_t now_ms);
FriendsState   friendsGetState();
uint32_t       friendsStateStartedAt();    // ms when current state began
uint8_t        friendsPeerAnimal();        // valid only in Matched (legacy)
uint32_t       friendsSearchSecondsLeft(); // for the countdown UI

// Send an item-broadcast on ESP-NOW. Caller checks rate-limits / counts.
void           friendsSendItem(uint8_t kind);

// Called by the caller (main.cpp) as soon as our own 5 sends are done.
// Sends a "session-done" burst so the partner knows: I'm done tapping.
// Both pets stay in Sending and keep receiving until EITHER both are
// done OR the 60 s session timeout fires — prevents the problem where
// the fast tapper friendsEnd's before the slow one has even sent its
// items.
void           friendsSignalDone();
bool           friendsLocalDone();
bool           friendsRemoteDone();

// Rendezvous: the local user pressed the "Meet up" button. Sends an
// ESP-NOW Ready broadcast and remembers the local time. A match happens
// when a Remote-Ready also arrives within the window (or already did) —
// then transition to Sending. Only callable in the Rendezvous state,
// otherwise no-op.
void           friendsTriggerRendezvous();
// UI helpers: was the local / remote Ready within the last
// kFriendsRendezvousWindowMs?
bool           friendsLocalReadyActive();
bool           friendsRemoteReadyActive();

// Snapshot of the receive queue collected during the last Sending session.
// Cleared by friendsClearRxQueue() once main.cpp finishes the playback.
uint8_t              friendsRxItemCount();
const FriendsRxItem* friendsRxItem(uint8_t i);
void                 friendsClearRxQueue();

// ─── Parent server (HTTP stats + session-limit form) ────────────────────
//
// Opt-in via Settings → "Eltern-Server". Keeps WiFi connected and runs an
// HTTP server on port 80 + mDNS "goo-goo.local". Battery-heavy; the UI
// tells the user as much. Stats are pushed in via parentServerSetStats()
// from the main loop; the renderer reads parentServerGetState() / IP.

enum class ParentServerState : uint8_t {
  Off,
  Connecting,
  Running,
  Failed,
};

struct ParentServerStats {
  uint8_t   batteryPct;
  uint8_t   charging;        // 0/1 instead of bool — keeps the struct trivially copyable
  uint32_t  sessionMin;      // current play session minutes
  uint32_t  totalMin;        // lifetime play minutes
  uint8_t   animal;          // 0 Bear, 1 Cat, 2 Dog
  uint8_t   language;        // 0 DE, 1 EN
  uint8_t   sessionLimitMin; // currently configured limit (mirrors Persisted)
};

void               parentServerEnable();
void               parentServerDisable();
void               parentServerTick();
ParentServerState  parentServerGetState();
const char*        parentServerGetIp();    // "192.168.x.y" once Running
void               parentServerSetStats(const ParentServerStats& s);

// True for one tick after the user submits the form. Caller (main.cpp)
// should read parentServerPendingSessionLimit() then write it to the
// Persisted struct.
bool    parentServerHasPendingLimit();
uint8_t parentServerPendingLimit();
void    parentServerClearPendingLimit();
