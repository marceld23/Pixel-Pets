#include "net.h"
#include "pip_link.h"

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <M5Unified.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sntp.h>

#include "target_caps.h"

namespace {

constexpr const char* kAPName = TARGET_AP_NAME;
constexpr uint32_t    kSetupTimeoutMs        = 5UL * 60UL * 1000UL;   // auto-cancel
constexpr uint32_t    kConnectTestTimeoutMs  = 12000;
constexpr uint32_t    kSubmittedHoldMs       = 500;
// Europe/Berlin POSIX TZ — handles CET/CEST DST transitions automatically.
// Canonical glibc form (no explicit DST offset → defaults to std+1, no
// explicit start time → defaults to 02:00). Tests showed that explicit
// "-2" / "/2" forms are misparsed by some ESP-IDF builds and end up
// applying ~+8h instead of +1/+2, so we intentionally stick to the
// minimal spelling.
constexpr const char* kPosixTzBerlin         = "CET-1CEST,M3.5.0,M10.5.0/3";

// Cached known-network list.
WifiCred  g_slots[kMaxWifiSlots];
uint8_t   g_slotsCount    = 0;
WiFiMulti g_wifiMulti;
bool      g_multiPopulated = false;
bool      g_eventHandlerRegistered = false;

// Diagnostics hook. Logs the ESP-IDF disconnect reason — Arduino's
// WiFi.status() only returns the generic "6" for all disconnects; the
// real reason (auth fail / handshake timeout / assoc fail / AP gone) is
// only in the event payload. An earlier iteration also set
// pmf_cfg.capable=false here as a workaround for ESP32-S3 + WPA2/WPA3-
// mixed APs — that turned out to be counter-productive on Fritz!Box guest
// networks (triggers AUTH_FAIL), so it was removed.
static void onStaWiFiEvent(arduino_event_t* ev) {
  if (!ev) return;
  if (ev->event_id == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("[wifi] disconnected reason=%d\n",
                  (int)ev->event_info.wifi_sta_disconnected.reason);
  }
}

// Setup-mode state machine.
WifiSetupState g_setupState     = WifiSetupState::Idle;
uint32_t       g_setupStartMs   = 0;
uint32_t       g_submittedAtMs  = 0;
uint32_t       g_connectStartMs = 0;
char           g_pendingSsid[33] = {0};
char           g_pendingPsk[65]  = {0};

DNSServer  g_dns;
WebServer  g_web(80);
bool       g_serversRunning = false;

int        g_scanCount = -1;     // -1 = none / running, >= 0 = result count

// ── Friends mode (ESP-NOW peer discovery) ─────────────────────────────
//
// Pets find each other directly via ESP-NOW broadcast on a fixed channel.
// No AP / router needed — works wherever two devices have line-of-sight
// (or close-by RF). Same 16-byte GOOG packet format as before.

constexpr uint32_t kFriendsSearchMs    = 60000;
constexpr uint32_t kFriendsMatchMs     = 10000;
constexpr uint32_t kFriendsNoFriendMs  =  2500;
constexpr uint32_t kFriendsAnnouncePer = 1000;
constexpr uint8_t  kMsgAnnounce        = 0;
constexpr uint8_t  kMsgMatch           = 1;

FriendsState g_friendsState     = FriendsState::Idle;
uint32_t     g_friendsStateAt   = 0;
uint32_t     g_friendsLastSendAt = 0;
uint8_t      g_friendsMyAnimal  = 0;
uint8_t      g_friendsMyLang    = 0;
uint8_t      g_friendsMyId[4]   = {0};
uint8_t      g_friendsPeerAnimal = 0;
bool         g_friendsHasMyId   = false;
bool         g_friendsEspNowReady = false;
uint32_t     g_friendsTxCount    = 0;
uint32_t     g_friendsRxCount    = 0;
uint32_t     g_friendsLastHeartbeatMs = 0;

// Receive ring buffer — recv callback runs in WiFi task context; main
// loop drains the queue in friendsTick(). Single-producer / single-
// consumer with volatile head/tail is safe without locks.
struct EspNowRxItem { uint8_t mac[6]; uint8_t data[32]; uint8_t len; };
constexpr uint8_t kRxQueueSize = 8;
EspNowRxItem      g_rxQueue[kRxQueueSize];
volatile uint8_t  g_rxHead = 0;
volatile uint8_t  g_rxTail = 0;

// Items received during the current Sending session. Frozen once full
// (5 max) — additional items beyond the cap are dropped.
FriendsRxItem g_friendsRx[kFriendsMaxRxItems];
uint8_t       g_friendsRxItemCount = 0;

// Item-burst dedup: each tap sends 3 packets with the same event-id
// (see friendsSendItem). The receiver remembers the last 5 eids and
// ignores duplicates. Without dedup all 3 burst packets would arrive as
// 3 separate gifts — and the cap of 5 would be full after 2 taps. eid=0
// is a sentinel for "no burst tracking" (e.g. Ready/Done) and is not
// dedup'd.
uint32_t      g_friendsRecentEids[kFriendsMaxRxItems] = {0};
uint8_t       g_friendsRecentEidIdx = 0;

// Outbox: locally sent items. Re-broadcast round-robin every 250 ms in the
// sending tick until bothDone. Reason: with asymmetric RF reception the
// partner pet does hear the repeated Done (re-broadcast every 500 ms),
// but the one-off 60 ms item bursts get physically swallowed. Repeated
// re-broadcast gives the receiver many independent chances — dedup
// (see above) prevents duplicate gifts.
struct FriendsTxOutboxItem {
  uint8_t  kind;
  uint32_t eid;
};
FriendsTxOutboxItem g_friendsTxOutbox[kFriendsMaxSends];
uint8_t             g_friendsTxOutboxCount = 0;
uint8_t             g_friendsTxOutboxRotIdx = 0;
uint32_t            g_friendsLastItemRebroadcastMs = 0;

static bool friendsEidSeenOrRecord(uint32_t eid) {
  if (eid == 0) return false;
  for (uint8_t i = 0; i < kFriendsMaxRxItems; ++i) {
    if (g_friendsRecentEids[i] == eid) return true;
  }
  g_friendsRecentEids[g_friendsRecentEidIdx] = eid;
  g_friendsRecentEidIdx =
      (uint8_t)((g_friendsRecentEidIdx + 1) % kFriendsMaxRxItems);
  return false;
}

// Session-done handshake: so the fast tapper doesn't friendsEnd while
// the slow one is still sending, both pets explicitly signal when they
// have all 5 sends through. The session ends only when local AND remote
// are done — OR the 60 s session timeout fires.
constexpr uint8_t  kMsgSessionDone = 7;
bool               g_friendsLocalDone     = false;
bool               g_friendsRemoteDone    = false;
uint32_t           g_friendsLocalDoneAt   = 0;

// ── Rendezvous state ──────────────────────────────────────────────────────
// Both pets must tap the big "Meet up" button within this window —
// otherwise it doesn't count. Hard 60 s timeout for the whole rendezvous
// phase, after which NoFriend.
constexpr uint32_t kFriendsRendezvousWindowMs  = 5000;
constexpr uint32_t kFriendsRendezvousTimeoutMs = 60000;
constexpr uint8_t  kMsgRendezvousReady         = 1;   // formerly kMsgMatch — repurposed
uint32_t           g_friendsLocalReadyMs       = 0;
uint32_t           g_friendsRemoteReadyMs      = 0;

// ── Parent server ─────────────────────────────────────────────────────
constexpr uint16_t kParentPort         = 80;
constexpr uint32_t kParentConnectMs    = 10000;
WebServer         g_parentWeb(kParentPort);
ParentServerState g_parentState        = ParentServerState::Off;
uint32_t          g_parentStateAt      = 0;
ParentServerStats g_parentStats        = {};
bool              g_parentMdnsRunning  = false;
bool              g_parentRunning      = false;
char              g_parentIp[20]       = {0};
bool              g_parentPendingLimit = false;
uint8_t           g_parentNewLimit     = 30;
uint32_t          g_parentLastReqMs    = 0;
constexpr uint32_t kParentIdleOffMs    = 30UL * 60UL * 1000UL;   // 30 min

}  // namespace

// Public broadcast-radio constants (declared in net.h, defined outside the
// anonymous namespace so they have external linkage).
const uint8_t kEspNowBroadcastMac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
const uint8_t kEspNowProtoMagic[4]   = {'G','O','O','G'};

// ─── Credentials (multi-slot) ────────────────────────────────────────────

bool             wifiHasCreds()    { return g_slotsCount > 0; }
uint8_t          wifiSlotsCount()  { return g_slotsCount; }
const WifiCred*  wifiSlot(uint8_t i) {
  return (i < g_slotsCount) ? &g_slots[i] : nullptr;
}
const char*      wifiCachedSsid()  {
  return (g_slotsCount > 0) ? g_slots[0].ssid : "";
}

bool wifiLoadCreds() {
  g_slotsCount = 0;
  Preferences p;
  if (!p.begin("wifi", true)) return false;
  uint8_t count = p.getUChar("count", 0);
  if (count > kMaxWifiSlots) count = kMaxWifiSlots;
  for (uint8_t i = 0; i < count; ++i) {
    char keyS[10], keyP[10];
    snprintf(keyS, sizeof(keyS), "ssid_%u", i);
    snprintf(keyP, sizeof(keyP), "psk_%u",  i);
    String ssid = p.getString(keyS, "");
    String psk  = p.getString(keyP, "");
    if (ssid.length() == 0) continue;
    strncpy(g_slots[g_slotsCount].ssid, ssid.c_str(),
            sizeof(g_slots[0].ssid) - 1);
    g_slots[g_slotsCount].ssid[sizeof(g_slots[0].ssid) - 1] = '\0';
    strncpy(g_slots[g_slotsCount].psk, psk.c_str(),
            sizeof(g_slots[0].psk) - 1);
    g_slots[g_slotsCount].psk[sizeof(g_slots[0].psk) - 1] = '\0';
    Serial.printf("[wifi] loaded slot %u: ssid='%s' (len %u) pskLen=%u\n",
                  (unsigned)g_slotsCount,
                  g_slots[g_slotsCount].ssid,
                  (unsigned)strlen(g_slots[g_slotsCount].ssid),
                  (unsigned)strlen(g_slots[g_slotsCount].psk));
    g_slotsCount++;
  }
  p.end();
  return g_slotsCount > 0;
}

static void saveAllSlots() {
  Preferences p;
  if (!p.begin("wifi", false)) {
    Serial.println(F("[wifi] saveAllSlots: prefs.begin FAILED"));
    return;
  }
  p.clear();
  p.putUChar("count", g_slotsCount);
  for (uint8_t i = 0; i < g_slotsCount; ++i) {
    char keyS[10], keyP[10];
    snprintf(keyS, sizeof(keyS), "ssid_%u", i);
    snprintf(keyP, sizeof(keyP), "psk_%u",  i);
    size_t sw = p.putString(keyS, g_slots[i].ssid);
    size_t pw = p.putString(keyP, g_slots[i].psk);
    Serial.printf("[wifi] saved slot %u: ssid='%s' (len %u, wrote %u) "
                  "pskLen=%u (wrote %u)\n",
                  (unsigned)i, g_slots[i].ssid,
                  (unsigned)strlen(g_slots[i].ssid), (unsigned)sw,
                  (unsigned)strlen(g_slots[i].psk), (unsigned)pw);
  }
  p.end();
}

void wifiAddCreds(const char* ssid, const char* psk) {
  if (!ssid || !ssid[0]) return;
  // Replace existing entry if SSID already known.
  for (uint8_t i = 0; i < g_slotsCount; ++i) {
    if (strncmp(g_slots[i].ssid, ssid, sizeof(g_slots[0].ssid)) == 0) {
      strncpy(g_slots[i].psk, psk ? psk : "", sizeof(g_slots[0].psk) - 1);
      g_slots[i].psk[sizeof(g_slots[0].psk) - 1] = '\0';
      saveAllSlots();
      g_multiPopulated = false;        // force re-add to WiFiMulti
      return;
    }
  }
  // New entry — append, or drop oldest (slot 0) if full.
  uint8_t target;
  if (g_slotsCount < kMaxWifiSlots) {
    target = g_slotsCount++;
  } else {
    for (uint8_t i = 0; i < kMaxWifiSlots - 1; ++i) g_slots[i] = g_slots[i + 1];
    target = kMaxWifiSlots - 1;
  }
  strncpy(g_slots[target].ssid, ssid, sizeof(g_slots[0].ssid) - 1);
  g_slots[target].ssid[sizeof(g_slots[0].ssid) - 1] = '\0';
  strncpy(g_slots[target].psk, psk ? psk : "", sizeof(g_slots[0].psk) - 1);
  g_slots[target].psk[sizeof(g_slots[0].psk) - 1] = '\0';
  saveAllSlots();
  g_multiPopulated = false;
}

void wifiClearCreds() {
  Preferences p;
  if (p.begin("wifi", false)) {
    p.clear();
    p.end();
  }
  g_slotsCount = 0;
  // WiFiMulti has its own AP list; fresh-init by re-creating it next boot.
  g_multiPopulated = false;
}

// ─── Boot-time STA + NTP ────────────────────────────────────────────────

static void populateMulti() {
  if (g_multiPopulated) return;
  // No straightforward "clear" on WiFiMulti — re-adding overwrites entries,
  // and we always populate from the canonical g_slots list, so this is fine
  // even if previous entries linger.
  for (uint8_t i = 0; i < g_slotsCount; ++i) {
    g_wifiMulti.addAP(g_slots[i].ssid, g_slots[i].psk);
  }
  g_multiPopulated = true;
}

void wifiBeginAsync() {
  if (g_slotsCount == 0) return;
  if (!g_eventHandlerRegistered) {
    WiFi.onEvent(onStaWiFiEvent);
    g_eventHandlerRegistered = true;
  }
  WiFi.mode(WIFI_STA);
  populateMulti();
  // WiFiMulti.run() is blocking; the actual connect happens inside
  // wifiSyncTimeAndDisconnect() so the splash can keep animating.
}

// Fallback time sync via the HTTP Date header. Used when SNTP (UDP/123)
// is blocked by captive portals / guest networks — HTTP usually works.
// We do a lightweight GET on a domain with a reliable Date header, parse
// it, set the system clock + RTC.
//
// The Date header is in RFC-7231 IMF format: "Mon, 01 May 2026 12:34:56 GMT".
// Second precision, which is enough for our purposes (RTC sync).
static bool parseImfDate(const char* s, struct tm* out) {
    static const char* kMon[12] = {
        "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
    };
    if (!s || strlen(s) < 25) return false;
    // Format: Mon, 01 May 2026 12:34:56 GMT
    //         0123456789012345678901234567
    char monStr[4] = {s[8], s[9], s[10], 0};
    int mon = -1;
    for (int i = 0; i < 12; ++i) {
        if (strcmp(monStr, kMon[i]) == 0) { mon = i; break; }
    }
    if (mon < 0) return false;
    int day  = atoi(s + 5);
    int year = atoi(s + 12);
    int hour = atoi(s + 17);
    int min  = atoi(s + 20);
    int sec  = atoi(s + 23);
    if (year < 2024 || year > 2100) return false;
    out->tm_year = year - 1900;
    out->tm_mon  = mon;
    out->tm_mday = day;
    out->tm_hour = hour;
    out->tm_min  = min;
    out->tm_sec  = sec;
    out->tm_isdst = 0;   // GMT, no DST
    return true;
}

static bool tryHttpDate(const char* url, String& outDate) {
    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(4000);
    if (!http.begin(url)) return false;
    const char* hdrs[] = {"Date"};
    http.collectHeaders(hdrs, 1);
    int code = http.GET();
    if (code <= 0) {
        Serial.printf("[time] http-date GET %s failed: %d\n", url, code);
        http.end();
        return false;
    }
    outDate = http.header("Date");
    http.end();
    return outDate.length() >= 25;
}

static bool fetchTimeFromHttp() {
    // Two endpoints in a row — both reliably return a Date header, both
    // are reachable over HTTP/80 (no TLS, no captive-portal quirk).
    // google.com/generate_204 is Android's captive-portal detect endpoint
    // and always answers with 204 + Date header. On a connection reset on
    // one we fall back to the next.
    static const char* kEndpoints[] = {
        "http://www.google.com/generate_204",
        "http://connectivitycheck.gstatic.com/generate_204",
        "http://detectportal.firefox.com/success.txt",
    };
    String dateStr;
    bool gotDate = false;
    for (auto* url : kEndpoints) {
        if (tryHttpDate(url, dateStr)) { gotDate = true; break; }
    }
    if (!gotDate) {
        Serial.println(F("[time] all http-date endpoints failed"));
        return false;
    }
    Serial.printf("[time] http-date raw: '%s'\n", dateStr.c_str());
    struct tm tmUtc{};
    if (!parseImfDate(dateStr.c_str(), &tmUtc)) {
        Serial.println(F("[time] http-date parse failed"));
        return false;
    }
    // mktime interprets tm as local, but we have UTC. Trick: temporarily
    // set TZ to UTC, call mktime, then set TZ back to Berlin.
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t epoch = mktime(&tmUtc);
    setenv("TZ", kPosixTzBerlin, 1);
    tzset();
    if (epoch <= 0) return false;
    struct timeval tv{};
    tv.tv_sec  = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    Serial.printf("[time] http-date set epoch=%lu (%04d-%02d-%02d %02d:%02d:%02d UTC)\n",
                  (unsigned long)epoch,
                  tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
                  tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec);
    return true;
}

static bool setRtcFromUnixTime(time_t t) {
  // Re-apply TZ right before the conversion. Some ESP-IDF builds reset
  // TZ when the SNTP task fires, and we've seen the RTC end up at UTC+8
  // when this happens. Setting again here is cheap insurance.
  setenv("TZ", kPosixTzBerlin, 1);
  tzset();

  struct tm utcInfo, timeinfo;
  gmtime_r(&t, &utcInfo);
  if (!localtime_r(&t, &timeinfo)) return false;
  Serial.printf(
    "[time] UTC %04d-%02d-%02d %02d:%02d  ->  local %02d:%02d  "
    "(TZ='%s', isdst=%d)\n",
    utcInfo.tm_year + 1900, utcInfo.tm_mon + 1, utcInfo.tm_mday,
    utcInfo.tm_hour, utcInfo.tm_min,
    timeinfo.tm_hour, timeinfo.tm_min,
    getenv("TZ") ? getenv("TZ") : "(null)",
    timeinfo.tm_isdst);

  m5::rtc_datetime_t dt;
  dt.date.year    = (uint16_t)(timeinfo.tm_year + 1900);
  dt.date.month   = (uint8_t)(timeinfo.tm_mon + 1);
  dt.date.date    = (uint8_t)timeinfo.tm_mday;
  dt.date.weekDay = (uint8_t)timeinfo.tm_wday;
  dt.time.hours   = (uint8_t)timeinfo.tm_hour;
  dt.time.minutes = (uint8_t)timeinfo.tm_min;
  dt.time.seconds = (uint8_t)timeinfo.tm_sec;
  M5.Rtc.setDateTime(&dt);
  return true;
}

bool wifiConnect(uint32_t connectTimeoutMs) {
  if (g_slotsCount == 0) return false;
  populateMulti();
  uint8_t status = g_wifiMulti.run(connectTimeoutMs);
  if (status != WL_CONNECTED) {
    // Erster Connect fehlgeschlagen. Auf Fritz!Box-Gastzugang oft, weil
    // Mesh-Repeater dieselbe SSID auf mehreren BSSIDs broadcasten und der
    // WiFiMulti-Scan einen schwachen (RSSI < -78) Knoten greift, der die
    // Assoc nicht durchbringt. Fresh disconnect + neuen Scan probieren —
    // beim zweiten Lauf landet die Heuristik gerne auf einem anderen AP.
    Serial.printf("[wifi] first connect attempt failed (status=%u, RSSI=%d) — retry\n",
                  (unsigned)status, WiFi.RSSI());
    WiFi.disconnect(true, false);
    delay(300);
    status = g_wifiMulti.run(connectTimeoutMs);
  }
  if (status != WL_CONNECTED) {
    Serial.printf("[wifi] connect gave up (status=%u)\n", (unsigned)status);
    // No WIFI_OFF — otherwise the next reconnect to the same BSSID would
    // take >10 s with NO_AP_FOUND. Stack stays awake in STA mode.
    WiFi.disconnect(true, false);
    return false;
  }
  return true;
}

bool wifiSyncTime(uint32_t ntpTimeoutMs) {
  if (!wifiIsConnected()) return false;
  // Diagnostics: log IP/gateway/DNS + try a pre-resolve. If DNS for
  // pool.ntp.org already fails here, NTP doesn't stand a chance.
  IPAddress localIp = WiFi.localIP();
  IPAddress gw      = WiFi.gatewayIP();
  IPAddress dns     = WiFi.dnsIP();
  Serial.printf("[time] ip=%s gw=%s dns=%s rssi=%d\n",
                localIp.toString().c_str(),
                gw.toString().c_str(),
                dns.toString().c_str(),
                WiFi.RSSI());
  IPAddress poolIp;
  uint32_t dnsStart = millis();
  bool dnsOk = WiFi.hostByName("pool.ntp.org", poolIp);
  Serial.printf("[time] DNS pool.ntp.org → %s after %lu ms (%s)\n",
                dnsOk ? poolIp.toString().c_str() : "—",
                (unsigned long)(millis() - dnsStart),
                dnsOk ? "OK" : "FAIL");

  // Reset SNTP state so we can detect a *fresh* sync. M5.begin pre-loads
  // the system clock from the BM8563 RTC, so the system time is already
  // > 2024 before NTP runs — that fooled our previous wait loop into
  // thinking sync was done and made us re-write the stale fallback time
  // back to the RTC.
  //
  // Three servers in this order:
  //   1. Default gateway — many home routers (Fritz!Box, Speedport, …)
  //      work as a local NTP server. Works even when the internet
  //      connection blocks port 123/UDP outbound.
  //   2. pool.ntp.org — the general standard.
  //   3. time.cloudflare.com — DNS anycast, often faster than pool.
  char gwStr[16];
  snprintf(gwStr, sizeof(gwStr), "%s", gw.toString().c_str());
  sntp_stop();
  configTzTime(kPosixTzBerlin, gwStr, "pool.ntp.org",
               "time.cloudflare.com");
  setenv("TZ", kPosixTzBerlin, 1);
  tzset();

  uint32_t start = millis();
  while (millis() - start < ntpTimeoutMs) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) break;
    delay(100);
  }
  bool synced = (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
  Serial.printf("[time] sntp sync %s after %lu ms (servers: %s, "
                "pool.ntp.org, time.cloudflare.com)\n",
                synced ? "OK" : "TIMEOUT",
                (unsigned long)(millis() - start), gwStr);
  if (!synced) {
    // SNTP fallback: many captive portals / guest networks block UDP/123
    // but leave HTTP/80 open. The Date header of any HTTP host gives us
    // the time to second precision — plenty for RTC sync.
    Serial.println(F("[time] trying HTTP-date fallback..."));
    synced = fetchTimeFromHttp();
  }
  if (synced) {
    time_t now = time(nullptr);
    setRtcFromUnixTime(now);
  }
  // Stay connected — caller will call wifiPowerOff().
  return synced;
}

bool wifiSyncTimeKeepConnected(uint32_t connectTimeoutMs,
                               uint32_t ntpTimeoutMs) {
  // Convenience wrapper: connect + time sync in one go.
  if (!wifiConnect(connectTimeoutMs)) return false;
  return wifiSyncTime(ntpTimeoutMs);
}

void wifiPowerOff() {
  // Previously: WiFi.mode(WIFI_OFF) — saved the most power, but put the
  // ESP32 WiFi stack into a state where a reconnect to the same BSSID
  // within 10-30 s reliably fails with NO_AP_FOUND (symptom: web radio
  // finds no WiFi even though the scan lists the AP). Instead: only end
  // the current connection, STA stays active. Modem-sleep runs in the
  // background and reduces power use without the reconnect bug.
  WiFi.disconnect(true, false);
  WiFi.setSleep(WIFI_PS_MAX_MODEM);
}

bool wifiSyncTimeAndDisconnect(uint32_t connectTimeoutMs,
                               uint32_t ntpTimeoutMs) {
  bool ok = wifiSyncTimeKeepConnected(connectTimeoutMs, ntpTimeoutMs);
  wifiPowerOff();
  return ok;
}

// ─── Reference-counted WiFi keep-alive ────────────────────────────────────

namespace {
constexpr uint8_t kMaxKeepAliveReasons = 4;   // webradio + friends + parents + 1 reserve
const char*       g_keepAliveReasons[kMaxKeepAliveReasons] = {nullptr};

int findReason(const char* reason) {
  for (uint8_t i = 0; i < kMaxKeepAliveReasons; ++i) {
    if (g_keepAliveReasons[i] && strcmp(g_keepAliveReasons[i], reason) == 0)
      return (int)i;
  }
  return -1;
}
int findFreeSlot() {
  for (uint8_t i = 0; i < kMaxKeepAliveReasons; ++i) {
    if (g_keepAliveReasons[i] == nullptr) return (int)i;
  }
  return -1;
}
uint8_t activeRefCount() {
  uint8_t c = 0;
  for (uint8_t i = 0; i < kMaxKeepAliveReasons; ++i) {
    if (g_keepAliveReasons[i] != nullptr) c++;
  }
  return c;
}
}  // namespace

bool wifiKeepAlive(const char* reason) {
  if (!reason) return false;
  if (findReason(reason) >= 0) return false;   // dedup, schon registriert
  int slot = findFreeSlot();
  if (slot < 0) {
    Serial.printf("[wifi] keepAlive: no free slot for '%s'\n", reason);
    return false;
  }
  g_keepAliveReasons[slot] = reason;
  Serial.printf("[wifi] keepAlive +%s (refs=%u)\n", reason, activeRefCount());
  // When we take the first reference and no connect is active, bring
  // it up. The WiFi stack stays in STA mode after wifiPowerOff() → we
  // just need to kick off the connect. Blocking run() with 5 s so the
  // caller (e.g. webradio) typically already sees wifiIsConnected() ==
  // true on return; otherwise it polls further itself.
  if (activeRefCount() == 1 && !wifiIsConnected()) {
    if (WiFi.getMode() == WIFI_OFF) {
      wifiBeginAsync();
    }
    populateMulti();
    uint8_t st = g_wifiMulti.run(5000);
    Serial.printf("[wifi] keepAlive run() → status=%u\n", (unsigned)st);
  }
  // Disable modem-sleep while someone is actively using WiFi — otherwise
  // audio streams get bandwidth dropouts every ~100 ms (DTIM beacon) →
  // buffer underrun → stuttering. wifiPowerOff() re-enables it.
  WiFi.setSleep(false);
  return true;
}

bool wifiRelease(const char* reason) {
  if (!reason) return false;
  int idx = findReason(reason);
  if (idx < 0) return false;
  g_keepAliveReasons[idx] = nullptr;
  Serial.printf("[wifi] keepAlive -%s (refs=%u)\n", reason, activeRefCount());
  if (activeRefCount() == 0) {
    Serial.println(F("[wifi] no more refs — powering down"));
    wifiPowerOff();
  }
  return true;
}

bool wifiKeepAliveActive() { return activeRefCount() > 0; }
bool wifiIsConnected()     { return WiFi.status() == WL_CONNECTED; }

// ─── Captive portal: HTML ──────────────────────────────────────────────

static String htmlEscape(const String& s) {
  String out; out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    switch (c) {
      case '&': out += "&amp;";  break;
      case '<': out += "&lt;";   break;
      case '>': out += "&gt;";   break;
      case '"': out += "&quot;"; break;
      case '\'':out += "&#39;";  break;
      default:  out += c;
    }
  }
  return out;
}

static String renderForm(const char* status) {
  String html;
  html.reserve(3500);
  html += F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>" TARGET_NAME " WiFi Setup</title>"
    "<style>"
      "body{font-family:sans-serif;background:#1a0030;color:#eee;"
      "padding:20px;max-width:420px;margin:auto;}"
      "h1{color:#cda3ff;text-align:center;margin-top:0;}"
      "label{display:block;margin-top:14px;font-size:14px;color:#bbb;}"
      "input,select{width:100%;padding:10px;font-size:16px;box-sizing:border-box;"
      "border-radius:6px;border:1px solid #555;background:#3a1f55;color:#eee;}"
      "button{margin-top:20px;width:100%;padding:14px;background:#cda3ff;"
      "color:#1a0030;font-weight:bold;border:none;border-radius:6px;font-size:16px;}"
      ".status{margin-top:14px;padding:10px;background:#3a1f55;border-radius:6px;font-size:14px;}"
    "</style></head><body>"
    "<h1>" TARGET_NAME " WiFi</h1>"
    "<form method='POST' action='/save'>"
    "<label>WLAN auswaehlen / Choose network:</label>"
    "<select id='ssidlist' onchange=\"var t=this.options[this.selectedIndex].text;"
    "document.getElementById('ssid').value=t.split(' (')[0];\">"
    "<option value=''>-- bitte waehlen / please pick --</option>"
  );
  if (g_scanCount > 0) {
    for (int i = 0; i < g_scanCount && i < 30; ++i) {
      String ssid = WiFi.SSID(i);
      int    rssi = WiFi.RSSI(i);
      html += F("<option>");
      html += htmlEscape(ssid);
      html += F(" (");
      html += String(rssi);
      html += F(" dBm)</option>");
    }
  } else {
    html += F("<option>(Scan laeuft...)</option>");
  }
  html += F(
    "</select>"
    "<label>SSID (manuell):</label>"
    "<input type='text' id='ssid' name='ssid' maxlength='32' required>"
    "<label>Passwort / Password:</label>"
    "<input type='password' name='psk' maxlength='64'>"
    "<button type='submit'>Verbinden / Connect</button>"
    "</form>"
  );
  if (status && status[0]) {
    html += F("<p class='status'>");
    html += status;
    html += F("</p>");
  }
  html += F("</body></html>");
  return html;
}

static void handleRoot() {
  g_web.send(200, "text/html", renderForm(""));
}

static void handleSave() {
  String ssid = g_web.arg("ssid");
  String psk  = g_web.arg("psk");
  Serial.printf("[wifi] form post: ssid='%s' (raw len %u) "
                "pskLen(raw)=%u\n",
                ssid.c_str(), (unsigned)ssid.length(),
                (unsigned)psk.length());
  ssid.trim();
  if (ssid.length() == 0) {
    g_web.send(200, "text/html",
               renderForm("Fehler: SSID darf nicht leer sein."));
    return;
  }
  if (ssid.length() > 32) ssid = ssid.substring(0, 32);
  if (psk.length()  > 64) psk  = psk.substring(0, 64);
  Serial.printf("[wifi] form post (post-trim): ssid='%s' (len %u) pskLen=%u\n",
                ssid.c_str(), (unsigned)ssid.length(),
                (unsigned)psk.length());

  strncpy(g_pendingSsid, ssid.c_str(), sizeof(g_pendingSsid) - 1);
  g_pendingSsid[sizeof(g_pendingSsid) - 1] = '\0';
  strncpy(g_pendingPsk, psk.c_str(), sizeof(g_pendingPsk) - 1);
  g_pendingPsk[sizeof(g_pendingPsk) - 1] = '\0';

  String resp = F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='3;url=/'></head>"
    "<body style='font-family:sans-serif;text-align:center;padding:30px;"
    "background:#1a0030;color:#eee;'>"
    "<h2>" TARGET_NAME " verbindet...</h2>"
    "<p>Ergebnis siehst du auf dem Geraet.</p>"
    "</body></html>");
  g_web.send(200, "text/html", resp);

  g_setupState    = WifiSetupState::Submitted;
  g_submittedAtMs = millis();
}

static void handleNotFound() {
  g_web.sendHeader("Location", "/", true);
  g_web.send(302, "text/plain", "");
}

// ─── Captive portal: lifecycle ──────────────────────────────────────────

static void teardownServers() {
  if (!g_serversRunning) return;
  g_dns.stop();
  g_web.stop();
  g_serversRunning = false;
}

void wifiSetupBegin() {
  // If the previous attempt failed (Failed) or succeeded, the servers may
  // not be running yet, but the state would block the restart — reset it
  // cleanly here so the captive portal comes up fresh.
  if (g_setupState == WifiSetupState::Connecting ||
      g_setupState == WifiSetupState::Submitted) return;
  if (g_setupState != WifiSetupState::Idle) {
    teardownServers();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    g_setupState = WifiSetupState::Idle;
    g_scanCount  = -1;
    delay(50);
  }

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(kAPName);     // open
  delay(200);

  // Async scan now so the form has fresh results once it's loaded.
  WiFi.scanNetworks(true);
  g_scanCount = -1;

  g_dns.setErrorReplyCode(DNSReplyCode::NoError);
  g_dns.start(53, "*", IPAddress(192, 168, 4, 1));

  g_web.on("/",                    HTTP_GET,  handleRoot);
  g_web.on("/save",                HTTP_POST, handleSave);
  // Captive-portal probes used by Android / Apple / Windows.
  g_web.on("/generate_204",        HTTP_GET, handleRoot);
  g_web.on("/hotspot-detect.html", HTTP_GET, handleRoot);
  g_web.on("/connecttest.txt",     HTTP_GET, handleRoot);
  g_web.on("/redirect",            HTTP_GET, handleRoot);
  g_web.onNotFound(handleNotFound);
  g_web.begin();

  g_serversRunning = true;
  g_setupStartMs   = millis();
  g_setupState     = WifiSetupState::Active;
}

void wifiSetupCancel() {
  teardownServers();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  g_setupState = WifiSetupState::Idle;
  g_scanCount  = -1;
}

WifiSetupState wifiSetupGetState() { return g_setupState; }
const char*    wifiSetupPendingSsid() { return g_pendingSsid; }
uint8_t        wifiSetupScanCount() {
  return (g_scanCount < 0) ? 0xFF : (uint8_t)((g_scanCount > 255) ? 255 : g_scanCount);
}

void wifiSetupTick() {
  if (g_setupState == WifiSetupState::Idle) return;

  // Service DNS + web while servers are running.
  if (g_serversRunning) {
    g_dns.processNextRequest();
    g_web.handleClient();
  }
  // Pick up async scan completion.
  if (g_scanCount < 0) {
    int n = WiFi.scanComplete();
    if (n >= 0) g_scanCount = n;
  }

  switch (g_setupState) {
    case WifiSetupState::Active: {
      // 5-min auto-timeout.
      if (millis() - g_setupStartMs > kSetupTimeoutMs) {
        wifiSetupCancel();
      }
      break;
    }
    case WifiSetupState::Submitted: {
      // Hold the web server up briefly so the response gets through, then
      // tear it down and fire off the STA connect attempt.
      if (millis() - g_submittedAtMs >= kSubmittedHoldMs) {
        teardownServers();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(g_pendingSsid, g_pendingPsk);
        g_connectStartMs = millis();
        g_setupState     = WifiSetupState::Connecting;
      }
      break;
    }
    case WifiSetupState::Connecting: {
      if (WiFi.status() == WL_CONNECTED) {
        wifiAddCreds(g_pendingSsid, g_pendingPsk);
        // Reset SNTP state then wait for a *fresh* sync — same fix as
        // wifiSyncTimeKeepConnected, otherwise the BM8563-preloaded
        // system clock fakes us into thinking sync already happened.
        sntp_stop();
        configTzTime(kPosixTzBerlin, "pool.ntp.org", "time.nist.gov");
        setenv("TZ", kPosixTzBerlin, 1);
        tzset();
        uint32_t t0 = millis();
        while (millis() - t0 < 3000) {
          if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) break;
          delay(100);
        }
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
          setRtcFromUnixTime(time(nullptr));
        }
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        g_setupState = WifiSetupState::Success;
      } else if (millis() - g_connectStartMs > kConnectTestTimeoutMs) {
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        g_setupState = WifiSetupState::Failed;
      }
      break;
    }
    case WifiSetupState::Success:
    case WifiSetupState::Failed:
    case WifiSetupState::Idle:
      break;
  }
}

// ─── Friends mode (UDP discovery) ───────────────────────────────────────

static void friendsTransition(FriendsState s) {
  g_friendsState   = s;
  g_friendsStateAt = millis();
}

static void friendsLoadMyId() {
  if (g_friendsHasMyId) return;
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  // Use the bottom 4 bytes of the MAC as a stable per-device ID.
  g_friendsMyId[0] = mac[2];
  g_friendsMyId[1] = mac[3];
  g_friendsMyId[2] = mac[4];
  g_friendsMyId[3] = mac[5];
  g_friendsHasMyId = true;
}

// Receive callback — runs in WiFi task context. Keep it short: copy data
// and bail. Main loop drains the queue in friendsTick().
static volatile uint32_t g_rxCbCount = 0;
static void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len <= 0 || len > (int)sizeof(g_rxQueue[0].data)) return;
  uint8_t next = (uint8_t)((g_rxHead + 1) % kRxQueueSize);
  if (next == g_rxTail) return;     // queue full — drop oldest sender's loss
  EspNowRxItem& item = g_rxQueue[g_rxHead];
  memcpy(item.mac, mac, 6);
  memcpy(item.data, data, len);
  item.len = (uint8_t)len;
  g_rxHead = next;
  g_rxCbCount++;   // read by the main task, diagnostics only
}

static void onEspNowSend(const uint8_t* /*mac*/, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.printf("[friends] tx send-cb status=%d (fail)\n", (int)status);
  }
}

// eid: optional event-id for item-burst dedup. 0 = "no eid" (Ready/Done are
// idempotent and need no dedup). Items set eid=millis() per tap and send
// the burst (3x) with the same eid; receiver dedups against that.
static void friendsSendPacket(uint8_t msgType, uint32_t eid = 0) {
  uint8_t buf[kEspNowPacketLen] = {0};
  memcpy(buf, kEspNowProtoMagic, 4);
  buf[4]  = kEspNowProtoVersion;
  buf[5]  = msgType;
  buf[6]  = g_friendsMyId[0];
  buf[7]  = g_friendsMyId[1];
  buf[8]  = g_friendsMyId[2];
  buf[9]  = g_friendsMyId[3];
  buf[10] = g_friendsMyAnimal;
  buf[11] = g_friendsMyLang;
  buf[12] = (uint8_t)(eid >>  0);
  buf[13] = (uint8_t)(eid >>  8);
  buf[14] = (uint8_t)(eid >> 16);
  buf[15] = (uint8_t)(eid >> 24);

  esp_err_t r = esp_now_send(kEspNowBroadcastMac, buf, kEspNowPacketLen);
  g_friendsTxCount++;
  Serial.printf("[friends] tx#%lu type=%u id=%02X%02X%02X%02X eid=%lu result=%d\n",
                (unsigned long)g_friendsTxCount, msgType,
                buf[6], buf[7], buf[8], buf[9],
                (unsigned long)eid, (int)r);
}

bool openEspNowRadio(const char* logPrefix) {
  if (!logPrefix) logPrefix = "espnow";
  // Hard WiFi reset. On one of the two test pets the RX path stayed
  // completely dead after a normal disconnect+mode(STA) (rxCb=0 for 60 s,
  // but TX functional). Suspicion: the WiFi stack was still in a
  // DISCONNECTED-with-pending-reconnect state → the subsequent
  // esp_wifi_set_channel was swallowed and RX listened on some other
  // channel. Fix: stop the stack completely, take a breath, then start
  // fresh in STA.
  WiFi.disconnect(true, true);
  delay(50);
  esp_wifi_stop();
  delay(50);
  WiFi.mode(WIFI_STA);
  delay(50);
  // Hard-disable power save. WiFi.setSleep(false) silently returns false
  // when the STA stack is in Arduino's internal "not started" state — then
  // the PHY stays in the default WIFI_PS_MIN_MODEM, only listens in DTIM
  // intervals, and misses short item bursts. Going directly through the
  // ESP-IDF API bypasses that.
  esp_err_t psRes = esp_wifi_set_ps(WIFI_PS_NONE);
  Serial.printf("[%s] esp_wifi_set_ps(WIFI_PS_NONE)=%d\n",
                logPrefix, (int)psRes);
  // TX power explicitly to the maximum (84 = 21 dBm). With asymmetric
  // reception it's important that both pets transmit as loudly as possible
  // so the partner with the weaker RX can hear anything at all.
  esp_err_t txpRes = esp_wifi_set_max_tx_power(84);
  int8_t    txpReadback = 0;
  esp_wifi_get_max_tx_power(&txpReadback);
  Serial.printf("[%s] esp_wifi_set_max_tx_power(84)=%d → readback=%d\n",
                logPrefix, (int)txpRes, (int)txpReadback);
  esp_wifi_set_promiscuous(false);
  // Long-range mode: ESP32-proprietary PHY profile with ~12 dB better
  // receive sensitivity (at the cost of data rate, ~512 kbps instead of
  // 54 Mbps). With asymmetric RF reception between two pets this is the
  // decisive lever — a weak receiver suddenly has enough margin to decode
  // the partner's signal reliably. closeEspNowRadio reverts to BGN.
  esp_err_t protoRes =
      esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
  Serial.printf("[%s] esp_wifi_set_protocol(LR)=%d\n",
                logPrefix, (int)protoRes);
  // Pin the radio onto our shared discovery channel.
  esp_err_t chanRes =
      esp_wifi_set_channel(kEspNowChannel, WIFI_SECOND_CHAN_NONE);
  uint8_t            chanReadback = 0;
  wifi_second_chan_t chanSecond   = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&chanReadback, &chanSecond);
  Serial.printf(
    "[%s] esp_wifi_set_channel=%d → readback primary=%u second=%d\n",
    logPrefix, (int)chanRes, (unsigned)chanReadback, (int)chanSecond);
  return chanRes == ESP_OK && chanReadback == kEspNowChannel;
}

void closeEspNowRadio() {
  // PHY profile back to standard B/G/N — otherwise the next WiFi AP
  // connect (time sync, weather, parent server) can't talk to normal
  // 802.11 routers. set_protocol requires an active STA stack, so do it
  // before the disconnect.
  esp_wifi_set_protocol(WIFI_IF_STA,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

void friendsBegin(uint8_t myAnimal, uint8_t myLanguage) {
  if (g_friendsState != FriendsState::Idle) return;
  // The Pip-link listener (if active) reuses the same ESP-NOW stack we are
  // about to reset for the friends session. Pause it so the rebroadcast
  // bursts don't get routed into companion-event handlers, and so our
  // hard-reset doesn't pull the listener's recv-cb out from under it.
  // Phase 1: pip_link is a no-op skeleton, so this is harmless prep wiring.
  pip_link::pause();
  g_friendsMyAnimal = myAnimal;
  g_friendsMyLang   = myLanguage;
  friendsLoadMyId();

  openEspNowRadio("friends");

  if (esp_now_init() != ESP_OK) {
    Serial.println(F("[friends] esp_now_init failed"));
    friendsTransition(FriendsState::NoFriend);
    return;
  }
  esp_err_t recvCbRes = esp_now_register_recv_cb(onEspNowRecv);
  esp_err_t sendCbRes = esp_now_register_send_cb(onEspNowSend);
  Serial.printf("[friends] register_recv_cb=%d register_send_cb=%d\n",
                (int)recvCbRes, (int)sendCbRes);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kEspNowBroadcastMac, 6);
  peer.channel = kEspNowChannel;
  peer.encrypt = false;
  esp_err_t addRes = esp_now_add_peer(&peer);
  if (addRes != ESP_OK && addRes != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("[friends] esp_now_add_peer failed: %d\n", (int)addRes);
  }

  g_friendsEspNowReady = true;
  g_friendsLastSendAt  = 0;
  g_rxHead = g_rxTail = 0;
  g_friendsRxItemCount = 0;
  g_friendsLocalReadyMs  = 0;
  g_friendsRemoteReadyMs = 0;
  g_friendsLocalDone     = false;
  g_friendsRemoteDone    = false;
  g_friendsLocalDoneAt   = 0;
  memset(g_friendsRecentEids, 0, sizeof(g_friendsRecentEids));
  g_friendsRecentEidIdx  = 0;
  g_friendsTxOutboxCount = 0;
  g_friendsTxOutboxRotIdx = 0;
  g_friendsLastItemRebroadcastMs = 0;
  Serial.printf("[friends] ESP-NOW ready, channel=%u — entering Rendezvous\n",
                (unsigned)kEspNowChannel);
  // Rendezvous phase: both pets must tap the "Meet up" button.
  friendsTransition(FriendsState::Rendezvous);
}

void friendsEnd() {
  if (g_friendsEspNowReady) {
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_del_peer(kEspNowBroadcastMac);
    esp_now_deinit();
    g_friendsEspNowReady = false;
  }
  closeEspNowRadio();
  g_multiPopulated  = false;
  g_friendsPeerAnimal = 0;
  g_friendsTxCount = 0;
  g_friendsRxCount = 0;
  g_friendsLastHeartbeatMs = 0;
  g_rxHead = g_rxTail = 0;
  g_friendsLocalReadyMs  = 0;
  g_friendsRemoteReadyMs = 0;
  g_friendsLocalDone     = false;
  g_friendsRemoteDone    = false;
  g_friendsLocalDoneAt   = 0;
  memset(g_friendsRecentEids, 0, sizeof(g_friendsRecentEids));
  g_friendsRecentEidIdx  = 0;
  g_friendsTxOutboxCount = 0;
  g_friendsTxOutboxRotIdx = 0;
  g_friendsLastItemRebroadcastMs = 0;
  friendsTransition(FriendsState::Idle);
  // Resume the Pip-link listener if it was running. Phase 1: no-op.
  pip_link::resume();
}

FriendsState friendsGetState()        { return g_friendsState; }
uint32_t     friendsStateStartedAt()  { return g_friendsStateAt; }
uint8_t      friendsPeerAnimal()      { return g_friendsPeerAnimal; }

void friendsSendItem(uint8_t kind) {
  if (g_friendsState != FriendsState::Sending) return;
  if (kind != FriendsItemGift && kind != FriendsItemHeart &&
      kind != FriendsItemFood && kind != FriendsItemGame) return;
  // Burst of 3 packets with the same event-id. A single ESP-NOW broadcast
  // packet can be lost under asymmetric RF conditions — and with it the
  // whole gift. Statistically, 3 attempts are enough to compensate for
  // single-loss rates << 100%. The receiver dedups via the eid (see
  // friendsEidSeenOrRecord).
  uint32_t eid = millis();
  if (eid == 0) eid = 1;     // 0 is the sentinel for "no eid" (Ready/Done)
  for (int i = 0; i < 3; ++i) {
    friendsSendPacket(kind, eid);
    if (i < 2) delay(20);
  }
  // Outbox entry for periodic re-broadcast in the sending tick.
  if (g_friendsTxOutboxCount < kFriendsMaxSends) {
    g_friendsTxOutbox[g_friendsTxOutboxCount].kind = kind;
    g_friendsTxOutbox[g_friendsTxOutboxCount].eid  = eid;
    g_friendsTxOutboxCount++;
  }
  Serial.printf("[friends] item kind=%u eid=%lu sent (burst x3, outbox=%u)\n",
                (unsigned)kind, (unsigned long)eid,
                (unsigned)g_friendsTxOutboxCount);
}

void friendsSignalDone() {
  if (g_friendsState != FriendsState::Sending) return;
  if (g_friendsLocalDone) return;   // already sent
  g_friendsLocalDone   = true;
  g_friendsLocalDoneAt = millis();
  // Burst of 4 done packets so it gets through even with RF interference.
  // Periodic re-send afterwards in the sending tick.
  for (int i = 0; i < 4; ++i) {
    friendsSendPacket(kMsgSessionDone);
    delay(20);
  }
  Serial.printf("[friends] local done @%lu (5 sends complete)\n",
                (unsigned long)g_friendsLocalDoneAt);
}
bool friendsLocalDone()  { return g_friendsLocalDone; }
bool friendsRemoteDone() { return g_friendsRemoteDone; }

void friendsTriggerRendezvous() {
  if (g_friendsState != FriendsState::Rendezvous) return;
  g_friendsLocalReadyMs = millis();
  // Initial burst: 3 packets in quick succession, then the periodic
  // re-broadcast in friendsTick takes over. A single packet isn't enough
  // for ESP-NOW broadcast — no ACKs, no retries.
  for (int i = 0; i < 3; ++i) {
    friendsSendPacket(kMsgRendezvousReady);
    delay(20);
  }
  g_friendsLastSendAt = millis();
  Serial.printf("[friends] local rendezvous ready @%lu\n",
                (unsigned long)g_friendsLocalReadyMs);
}

bool friendsLocalReadyActive() {
  uint32_t r = g_friendsLocalReadyMs;
  return r != 0 && (millis() - r) < kFriendsRendezvousWindowMs;
}
bool friendsRemoteReadyActive() {
  uint32_t r = g_friendsRemoteReadyMs;
  return r != 0 && (millis() - r) < kFriendsRendezvousWindowMs;
}

uint8_t              friendsRxItemCount() { return g_friendsRxItemCount; }
const FriendsRxItem* friendsRxItem(uint8_t i) {
  return (i < g_friendsRxItemCount) ? &g_friendsRx[i] : nullptr;
}
void friendsClearRxQueue() {
  g_friendsRxItemCount = 0;
}

uint32_t friendsSearchSecondsLeft() {
  if (g_friendsState != FriendsState::Searching) return 0;
  uint32_t age = millis() - g_friendsStateAt;
  if (age >= kFriendsSearchMs) return 0;
  return (kFriendsSearchMs - age + 999) / 1000;
}

static bool friendsTryDecode(const uint8_t* data, size_t len, uint8_t* outType,
                             uint8_t outId[4], uint8_t* outAnimal,
                             uint32_t* outEid) {
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
  return true;
}

void friendsTick(uint32_t now_ms) {
  if (g_friendsState == FriendsState::Idle) return;

  switch (g_friendsState) {
    case FriendsState::Rendezvous: {
      // Drain the RX queue. We care about Ready packets (match trigger)
      // — BUT item and Done packets must already be accepted too: after
      // its own match the partner may already be sending items before we
      // switch from Rendezvous to Sending (loop-tick offset ~33 ms).
      // Otherwise the items are unrecoverably lost — in the worst case
      // all five, leaving one pet without gifts.
      while (g_rxTail != g_rxHead) {
        EspNowRxItem& item = g_rxQueue[g_rxTail];
        g_rxTail = (uint8_t)((g_rxTail + 1) % kRxQueueSize);
        uint8_t type, id[4], animal;
        uint32_t eid = 0;
        if (!friendsTryDecode(item.data, item.len, &type, id, &animal, &eid)) continue;
        if (memcmp(id, g_friendsMyId, 4) == 0) continue;     // own
        if (type == kMsgRendezvousReady) {
          g_friendsRemoteReadyMs = now_ms;
          g_friendsPeerAnimal    = animal;
          Serial.printf(
            "[friends] remote rendezvous ready (animal=%u) @%lu\n",
            animal, (unsigned long)now_ms);
          continue;
        }
        if (type == kMsgSessionDone) {
          g_friendsRemoteDone = true;
          Serial.println(F("[friends] remote done (early, in Rendezvous)"));
          continue;
        }
        if (type >= FriendsItemGift && type <= FriendsItemGame) {
          if (friendsEidSeenOrRecord(eid)) {
            Serial.printf(
              "[friends]   -> early item kind=%u eid=%lu DUP, dropped\n",
              type, (unsigned long)eid);
            continue;
          }
          if (g_friendsRxItemCount < kFriendsMaxRxItems) {
            g_friendsRx[g_friendsRxItemCount].kind         = type;
            g_friendsRx[g_friendsRxItemCount].senderAnimal = animal;
            g_friendsRxItemCount++;
            Serial.printf(
              "[friends]   -> early item kind=%u eid=%lu (in Rendezvous, slot %u/%u)\n",
              type, (unsigned long)eid,
              g_friendsRxItemCount, kFriendsMaxRxItems);
          }
        }
      }
      // Re-broadcast: ESP-NOW broadcasts are best-effort and can be lost
      // in one direction. While our local Ready is still in the match
      // window, re-send it ~every 400 ms — that gives the partner several
      // chances to hear it. Otherwise the symptom is: one pet matches
      // (sees the other's first Ready), the other gets stuck until the
      // 60 s timeout.
      if (g_friendsLocalReadyMs != 0 &&
          (int32_t)(now_ms - g_friendsLocalReadyMs) <
              (int32_t)kFriendsRendezvousWindowMs) {
        if ((int32_t)(now_ms - g_friendsLastSendAt) >= 400) {
          g_friendsLastSendAt = now_ms;
          friendsSendPacket(kMsgRendezvousReady);
        }
      }

      // Diagnostics: log status once per second — lets us see whether
      // and when packets arrive and what the match flags say.
      static uint32_t s_rzvDiag = 0;
      if (now_ms - s_rzvDiag >= 1000) {
        s_rzvDiag = now_ms;
        Serial.printf("[friends-rzv] now=%lu rxCb=%lu localR=%lu remoteR=%lu\n",
                      (unsigned long)now_ms,
                      (unsigned long)g_rxCbCount,
                      (unsigned long)g_friendsLocalReadyMs,
                      (unsigned long)g_friendsRemoteReadyMs);
      }
      // Match: both sides recently signalled Ready. Match window is
      // kFriendsRendezvousWindowMs.
      bool localReady  = g_friendsLocalReadyMs  != 0 &&
                         (now_ms - g_friendsLocalReadyMs)  < kFriendsRendezvousWindowMs;
      bool remoteReady = g_friendsRemoteReadyMs != 0 &&
                         (now_ms - g_friendsRemoteReadyMs) < kFriendsRendezvousWindowMs;
      if (localReady && remoteReady) {
        Serial.println(F("[friends] rendezvous matched — entering Sending"));
        // Match burst: 4 more Ready packets in quick succession so the
        // partner — whose last Ready to us could have been dropped by
        // RF loss — is guaranteed to hear at least one. Otherwise an
        // asymmetry: we match, stop sending, the partner hears nothing
        // more and waits the full 60 s into the timeout.
        for (int i = 0; i < 4; ++i) {
          friendsSendPacket(kMsgRendezvousReady);
          delay(20);   // ESP-NOW needs a few ms between bursts
        }
        friendsTransition(FriendsState::Sending);
        break;
      }
      // Timeout — no match in the allotted time. Signed compare,
      // because g_friendsStateAt is set in the same loop tick as the
      // friendsBegin call and can be marginally LATER than the now_ms
      // captured at the start of the loop (each individual millis()
      // call keeps ticking). Without the signed cast uint32_t would
      // underflow and ~4 billion would trip the 60 s threshold
      // immediately → instant timeout on the very first tick.
      if ((int32_t)(now_ms - g_friendsStateAt) >=
          (int32_t)kFriendsRendezvousTimeoutMs) {
        Serial.println(F("[friends] rendezvous timeout — NoFriend"));
        friendsTransition(FriendsState::NoFriend);
      }
      break;
    }
    case FriendsState::Sending: {
      if (now_ms - g_friendsLastHeartbeatMs >= 5000) {
        g_friendsLastHeartbeatMs = now_ms;
        // rxCb = ESP-NOW recv-callback calls (PHY level). rx = packets
        // drained by the main task. Diff (rxCb - rx) = queue overflow or
        // dropped packets. If rxCb=0 despite expected bursts → PHY
        // hears nothing (channel / power-save issue).
        Serial.printf(
          "[friends] heartbeat: tx=%lu rxCb=%lu rx=%lu items=%u state=Sending\n",
          (unsigned long)g_friendsTxCount,
          (unsigned long)g_rxCbCount,
          (unsigned long)g_friendsRxCount,
          (unsigned)g_friendsRxItemCount);
      }
      // Drain RX queue (filled by the ESP-NOW recv callback). Item-type
      // packets (2..5) get queued into the rx-items list (capped at 5);
      // anything else is ignored.
      while (g_rxTail != g_rxHead) {
        EspNowRxItem& item = g_rxQueue[g_rxTail];
        g_rxTail = (uint8_t)((g_rxTail + 1) % kRxQueueSize);
        g_friendsRxCount++;
        Serial.printf(
          "[friends] rx#%lu %u bytes from %02X:%02X:%02X:%02X:%02X:%02X\n",
          (unsigned long)g_friendsRxCount, item.len,
          item.mac[0], item.mac[1], item.mac[2],
          item.mac[3], item.mac[4], item.mac[5]);
        uint8_t type, id[4], animal;
        uint32_t eid = 0;
        if (!friendsTryDecode(item.data, item.len, &type, id, &animal, &eid)) {
          Serial.println(F("[friends]   -> not a GOOG packet, ignored"));
          continue;
        }
        if (memcmp(id, g_friendsMyId, 4) == 0) continue;     // own
        if (type == kMsgSessionDone) {
          if (!g_friendsRemoteDone) {
            Serial.println(F("[friends]   -> remote done"));
          }
          g_friendsRemoteDone = true;
          continue;
        }
        if (type < FriendsItemGift || type > FriendsItemGame) continue;
        if (friendsEidSeenOrRecord(eid)) {
          Serial.printf(
            "[friends]   -> item kind=%u eid=%lu DUP, dropped\n",
            type, (unsigned long)eid);
          continue;
        }
        if (g_friendsRxItemCount >= kFriendsMaxRxItems) {
          Serial.println(F("[friends]   -> rx queue full, dropping"));
          continue;
        }
        g_friendsRx[g_friendsRxItemCount].kind         = type;
        g_friendsRx[g_friendsRxItemCount].senderAnimal = animal;
        g_friendsRxItemCount++;
        Serial.printf(
          "[friends]   -> queued item kind=%u eid=%lu from animal=%u (slot %u/%u)\n",
          type, (unsigned long)eid, animal,
          g_friendsRxItemCount, kFriendsMaxRxItems);
      }
      // Done re-broadcast: as long as our local done flag is set,
      // resend the Done packet every ~250 ms — the partner may need
      // several attempts before one gets through. Keeps running even
      // after we've seen the partner's Done, because the main-loop
      // bothDone check now holds the Sending state open for a trailing
      // window so the *partner* can still catch our Done. 250 ms gives
      // ~8 chances within a 2 s trailing window.
      if (g_friendsLocalDone &&
          (int32_t)(now_ms - g_friendsLastSendAt) >= 250) {
        g_friendsLastSendAt = now_ms;
        friendsSendPacket(kMsgSessionDone);
      }
      // Item re-broadcast: with asymmetric RF reception the partner
      // pet hears the every-500-ms Done but not the one-off 60 ms item
      // bursts. Round-robin through our outbox every 250 ms until the
      // partner acks with Done that it's finished receiving
      // (g_friendsRemoteDone) — after that there's no point.
      if (g_friendsTxOutboxCount > 0 && !g_friendsRemoteDone &&
          (int32_t)(now_ms - g_friendsLastItemRebroadcastMs) >= 250) {
        g_friendsLastItemRebroadcastMs = now_ms;
        const FriendsTxOutboxItem& it =
            g_friendsTxOutbox[g_friendsTxOutboxRotIdx];
        g_friendsTxOutboxRotIdx =
            (uint8_t)((g_friendsTxOutboxRotIdx + 1) % g_friendsTxOutboxCount);
        friendsSendPacket(it.kind, it.eid);
      }
      // Note: session end is handled in main.cpp — either both done OR
      // the 60 s session timeout. The 5-tap cap (for the buttons) is
      // enforced in handleTouch.
      break;
    }
    // Legacy states — kept for FriendsView struct compat. Not entered.
    case FriendsState::Connecting:
    case FriendsState::Searching:
    case FriendsState::Matched:
      friendsTransition(FriendsState::Sending);
      break;
    case FriendsState::NoFriend: {
      if ((int32_t)(now_ms - g_friendsStateAt) >=
          (int32_t)kFriendsNoFriendMs) {
        friendsEnd();
      }
      break;
    }
    case FriendsState::Idle:
      break;
  }
}

// ─── Parent server ──────────────────────────────────────────────────────

static void parentTransition(ParentServerState s) {
  g_parentState   = s;
  g_parentStateAt = millis();
}

static String parentRenderHtml() {
  String h;
  h.reserve(2200);
  static const char* animals_de[] = { "Baer", "Katze", "Hund" };
  static const char* animals_en[] = { "Bear", "Cat",   "Dog"  };
  bool de = (g_parentStats.language == 0);
  const char* animalName = (g_parentStats.animal < 3)
                              ? (de ? animals_de[g_parentStats.animal]
                                    : animals_en[g_parentStats.animal])
                              : "?";
  h += F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='10'>"
    "<title>" TARGET_NAME "</title>"
    "<style>"
      "body{font-family:sans-serif;background:#1a0030;color:#eee;"
      "padding:20px;max-width:480px;margin:auto;}"
      "h1{color:#cda3ff;text-align:center;margin-top:0;}"
      "table{width:100%;border-collapse:collapse;margin-bottom:20px;}"
      "th,td{padding:8px 4px;text-align:left;border-bottom:1px solid #3a1f55;}"
      "th{color:#aaa;font-weight:normal;width:50%;}"
      "td{color:#fff;}"
      "form{background:#3a1f55;padding:14px;border-radius:8px;}"
      "label{display:block;margin-top:6px;color:#cda3ff;}"
      "input,select{width:100%;padding:8px;font-size:16px;box-sizing:border-box;"
      "border-radius:6px;border:1px solid #555;background:#2a1245;color:#eee;}"
      "button{margin-top:14px;width:100%;padding:12px;background:#cda3ff;"
      "color:#1a0030;font-weight:bold;border:none;border-radius:6px;font-size:16px;}"
      ".note{color:#aaa;font-size:12px;margin-top:4px;}"
    "</style></head><body>"
    "<h1>" TARGET_NAME "</h1><table>"
  );

  auto row = [&](const char* lbl, const String& val) {
    h += "<tr><th>"; h += lbl; h += "</th><td>"; h += val; h += "</td></tr>";
  };

  row(de ? "Tier" : "Pet", animalName);

  String batt = String(g_parentStats.batteryPct) + "%";
  if (g_parentStats.charging) batt += de ? " (laedt)" : " (charging)";
  row(de ? "Akku" : "Battery", batt);

  row(de ? "Sitzung (min)" : "Session (min)", String(g_parentStats.sessionMin));
  row(de ? "Gesamt (min)"  : "Total (min)",   String(g_parentStats.totalMin));
  row(de ? "Limit (min)"   : "Limit (min)",   String(g_parentStats.sessionLimitMin));

  h += F(
    "</table>"
    "<form method='POST' action='/save'>"
  );
  h += de ? F("<label>Spielzeit-Limit (Minuten):</label>")
          : F("<label>Play time limit (minutes):</label>");
  h += F("<select name='limit'>");
  static const uint8_t opts[] = { 5, 10, 15, 20, 30, 45, 60, 90, 120 };
  for (uint8_t v : opts) {
    h += "<option value='"; h += String(v); h += "'";
    if (v == g_parentStats.sessionLimitMin) h += " selected";
    h += ">"; h += String(v); h += "</option>";
  }
  h += F("</select>");
  h += de ? F("<button type='submit'>Speichern</button>")
          : F("<button type='submit'>Save</button>");
  h += F(
    "<p class='note'>"
  );
  h += de ? F("Aenderung wird beim naechsten Tippen am Pet wirksam. "
              "Seite aktualisiert sich alle 10 s.")
          : F("Change applies the next time the pet is touched. "
              "Page refreshes every 10 s.");
  h += F("</p></form></body></html>");
  return h;
}

static void parentHandleRoot() {
  g_parentLastReqMs = millis();
  g_parentWeb.send(200, "text/html; charset=utf-8", parentRenderHtml());
}

static void parentHandleSave() {
  g_parentLastReqMs = millis();
  String v = g_parentWeb.arg("limit");
  int n = v.toInt();
  if (n < 5)   n = 5;
  if (n > 120) n = 120;
  g_parentNewLimit     = (uint8_t)n;
  g_parentPendingLimit = true;
  // Reflect it in the locally-rendered stats immediately so the form
  // shows the new value even before main.cpp has saved to NVS.
  g_parentStats.sessionLimitMin = (uint8_t)n;
  // Redirect back to root so the user sees the table refresh.
  g_parentWeb.sendHeader("Location", "/", true);
  g_parentWeb.send(303, "text/plain", "");
}

static void parentStart() {
  if (g_parentRunning) return;
  g_parentLastReqMs = millis();   // start the idle clock
  IPAddress ip = WiFi.localIP();
  snprintf(g_parentIp, sizeof(g_parentIp), "%u.%u.%u.%u",
           ip[0], ip[1], ip[2], ip[3]);

  g_parentWeb.on("/",     HTTP_GET,  parentHandleRoot);
  g_parentWeb.on("/save", HTTP_POST, parentHandleSave);
  g_parentWeb.onNotFound(parentHandleRoot);
  g_parentWeb.begin();

  if (MDNS.begin(TARGET_MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    g_parentMdnsRunning = true;
  }
  g_parentRunning = true;
  parentTransition(ParentServerState::Running);
}

static void parentStop() {
  if (g_parentMdnsRunning) {
    MDNS.end();
    g_parentMdnsRunning = false;
  }
  if (g_parentRunning) {
    g_parentWeb.stop();
    g_parentRunning = false;
  }
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  g_multiPopulated = false;
  g_parentIp[0]    = 0;
  parentTransition(ParentServerState::Off);
}

void parentServerEnable() {
  if (g_parentState != ParentServerState::Off) return;
  if (g_slotsCount == 0) {
    parentTransition(ParentServerState::Failed);
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  // Same direct-begin pattern as friendsBegin — WiFiMulti.run(0) returns
  // failure immediately with no time to actually connect.
  WiFi.begin(g_slots[0].ssid, g_slots[0].psk);
  parentTransition(ParentServerState::Connecting);
}

void parentServerDisable() {
  parentStop();
}

ParentServerState parentServerGetState() { return g_parentState; }
const char*       parentServerGetIp()    { return g_parentIp; }

void parentServerSetStats(const ParentServerStats& s) {
  g_parentStats = s;
}

bool    parentServerHasPendingLimit()   { return g_parentPendingLimit; }
uint8_t parentServerPendingLimit()      { return g_parentNewLimit; }
void    parentServerClearPendingLimit() { g_parentPendingLimit = false; }

void parentServerTick() {
  switch (g_parentState) {
    case ParentServerState::Off:
    case ParentServerState::Failed:
      return;
    case ParentServerState::Connecting:
      if (WiFi.status() == WL_CONNECTED) {
        parentStart();
      } else if (millis() - g_parentStateAt > kParentConnectMs) {
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        parentTransition(ParentServerState::Failed);
      }
      return;
    case ParentServerState::Running:
      if (WiFi.status() != WL_CONNECTED) {
        // Lost connection — try to recover. WiFiMulti retries internally.
        if (g_parentRunning) {
          g_parentWeb.stop();
          if (g_parentMdnsRunning) { MDNS.end(); g_parentMdnsRunning = false; }
          g_parentRunning = false;
        }
        parentTransition(ParentServerState::Connecting);
        return;
      }
      g_parentWeb.handleClient();
      // Auto-shutdown after 30 min without any HTTP request — WiFi
      // continuously on is the single biggest battery drain.
      if (millis() - g_parentLastReqMs > kParentIdleOffMs) {
        Serial.println(F("[parent] 30 min idle — auto-disable"));
        parentStop();
      }
      return;
  }
}
