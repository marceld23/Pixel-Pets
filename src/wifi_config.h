#pragma once

// ─── Edit these with your WiFi credentials ──────────────────────────────────
//
// If WIFI_SSID is left empty (""), the pet skips NTP sync at boot and the
// time-of-day backgrounds fall back to the "Day" phase. So you can leave it
// blank for offline use.

#define WIFI_SSID      ""
#define WIFI_PASSWORD  ""

// POSIX TZ string. Default is Europe/Berlin (CET/CEST with EU DST rules).
// Reference: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
#define NTP_TZ_STRING  "CET-1CEST-2,M3.5.0/2,M10.5.0/3"

#define NTP_SERVER_1   "de.pool.ntp.org"
#define NTP_SERVER_2   "pool.ntp.org"
