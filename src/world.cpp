#include "world.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <math.h>

namespace {

// Location cache 24 h. Previously it was 7 days — looked elegant
// (IP location rarely changes) but was too lax: a once-cached
// wrong hit (CGNAT pop, ISP inaccuracy) would stick for a week.
// If the pet actually moves you'd want to refresh more often anyway —
// and fetchLocation costs a slim HTTP GET, not worth worrying about.
constexpr uint32_t kLocationMaxAgeSec = 24UL * 60UL * 60UL;        // 24 h
constexpr uint32_t kWeatherMaxAgeSec  = 60UL * 60UL;               // 1 hour

constexpr const char* kIpApiUrl =
  "http://ip-api.com/json/?fields=status,country,countryCode,city,lat,lon,timezone";

// We pin "/json/" (free tier, no key needed) on plain HTTP. The free tier
// is rate-limited but plenty for a once-a-week refresh.

WorldLocation g_loc{};
WorldWeather  g_weather{};
WorldMoon     g_moon{};
bool          g_freshThisSession = false;

// ─── NVS helpers ────────────────────────────────────────────────────────

void saveLocation() {
  Preferences p;
  if (!p.begin("world", false)) return;
  p.putBool ("locVal",  g_loc.valid);
  p.putString("locCity", g_loc.city);
  p.putString("locCC",   g_loc.countryCode);
  p.putFloat ("locLat",  g_loc.lat);
  p.putFloat ("locLon",  g_loc.lon);
  p.putString("locTz",   g_loc.timezone);
  p.putUInt  ("locUpd",  g_loc.lastUpdatedEpoch);
  p.end();
}

void saveWeather() {
  Preferences p;
  if (!p.begin("world", false)) return;
  p.putBool  ("wxVal",   g_weather.valid);
  p.putUShort("wxCode",  g_weather.weatherCode);
  p.putChar  ("wxTemp",  g_weather.temperatureC);
  p.putUChar ("wxHum",   g_weather.humidityPct);
  p.putUChar ("wxDay",   g_weather.isDay);
  p.putUChar ("wxWind",  g_weather.windKmh);
  p.putUChar ("wxPrcp",  g_weather.precipMmTimes10);
  p.putUInt  ("wxSr",    g_weather.sunriseUtc);
  p.putUInt  ("wxSs",    g_weather.sunsetUtc);
  p.putUInt  ("wxUpd",   g_weather.lastUpdatedEpoch);
  p.end();
}

// ─── Moon phase (synodic month approximation) ───────────────────────────
//
// Reference new-moon: 2000-01-06 18:14 UTC = unix 947182440. Synodic
// month ≈ 29.530588853 days. Good enough for "what mood the night has".

void calcMoon(uint32_t nowEpoch) {
  if (nowEpoch < 1700000000) {
    // RTC unset — leave previous values in place.
    return;
  }
  constexpr double kRefNewMoonEpoch = 947182440.0;
  constexpr double kSynodicSeconds  = 29.530588853 * 86400.0;
  double age = ((double)nowEpoch - kRefNewMoonEpoch);
  double cycles = age / kSynodicSeconds;
  cycles -= floor(cycles);          // 0..1 fraction along the cycle
  if (cycles < 0) cycles += 1.0;
  g_moon.phase01 = (float)cycles;

  // Buckets follow the user's plan (8 phases).
  MoonPhase ph;
  if      (cycles < 0.03 || cycles > 0.97) ph = MoonPhase::NewMoon;
  else if (cycles < 0.22)                  ph = MoonPhase::WaxingCrescent;
  else if (cycles < 0.28)                  ph = MoonPhase::FirstQuarter;
  else if (cycles < 0.47)                  ph = MoonPhase::WaxingGibbous;
  else if (cycles < 0.53)                  ph = MoonPhase::FullMoon;
  else if (cycles < 0.72)                  ph = MoonPhase::WaningGibbous;
  else if (cycles < 0.78)                  ph = MoonPhase::LastQuarter;
  else                                     ph = MoonPhase::WaningCrescent;
  g_moon.phase = ph;

  // Illumination ≈ (1 − cos(2π × phase)) / 2
  double illum = (1.0 - cos(cycles * 2.0 * M_PI)) * 0.5;
  if (illum < 0) illum = 0;
  if (illum > 1) illum = 1;
  g_moon.illuminationPct = (uint8_t)(illum * 100.0);

  static const char* kPhaseNames[8] = {
    "NewMoon", "WaxingCrescent", "FirstQuarter", "WaxingGibbous",
    "FullMoon", "WaningGibbous", "LastQuarter", "WaningCrescent",
  };
  Serial.printf("[world]   moon: %s phase01=%.3f illum=%u%%\n",
                kPhaseNames[(int)ph], g_moon.phase01,
                g_moon.illuminationPct);
}

// ─── HTTP fetch helpers ────────────────────────────────────────────────

bool httpGetJson(const char* url, JsonDocument& out, uint16_t timeoutMs) {
  HTTPClient http;
  http.setConnectTimeout(timeoutMs);
  http.setTimeout(timeoutMs);
  if (!http.begin(url)) {
    Serial.printf("[world] http.begin failed for %s\n", url);
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[world] HTTP %d for %s\n", code, url);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  DeserializationError err = deserializeJson(out, body);
  if (err) {
    Serial.printf("[world] JSON parse error: %s\n", err.c_str());
    return false;
  }
  return true;
}

bool fetchLocation() {
  JsonDocument doc;
  if (!httpGetJson(kIpApiUrl, doc, 4000)) return false;
  const char* status = doc["status"] | "";
  if (strcmp(status, "success") != 0) {
    Serial.printf("[world] ip-api status=%s\n", status);
    return false;
  }
  g_loc.valid = true;
  strncpy(g_loc.city, doc["city"] | "?", sizeof(g_loc.city) - 1);
  g_loc.city[sizeof(g_loc.city) - 1] = '\0';
  strncpy(g_loc.countryCode, doc["countryCode"] | "??",
          sizeof(g_loc.countryCode) - 1);
  g_loc.countryCode[sizeof(g_loc.countryCode) - 1] = '\0';
  g_loc.lat = doc["lat"] | 0.0f;
  g_loc.lon = doc["lon"] | 0.0f;
  strncpy(g_loc.timezone, doc["timezone"] | "Etc/UTC",
          sizeof(g_loc.timezone) - 1);
  g_loc.timezone[sizeof(g_loc.timezone) - 1] = '\0';
  Serial.printf("[world] location: %s, %s (%.3f, %.3f) tz=%s\n",
                g_loc.city, g_loc.countryCode, g_loc.lat, g_loc.lon,
                g_loc.timezone);
  return true;
}

// ISO time "2026-04-28T06:13" → unix seconds, parsed as if UTC. Open-meteo
// returns local times when timezone is set, so we deliberately interpret
// them as the local wall clock — combined with the device's configured TZ
// they're useful directly. We just return `time_t` of the wall-clock
// reading.
uint32_t parseIso8601LocalAsEpoch(const char* iso) {
  if (!iso || strlen(iso) < 16) return 0;
  struct tm tmv = {};
  // YYYY-MM-DDTHH:MM
  tmv.tm_year = atoi(iso) - 1900;
  tmv.tm_mon  = atoi(iso + 5) - 1;
  tmv.tm_mday = atoi(iso + 8);
  tmv.tm_hour = atoi(iso + 11);
  tmv.tm_min  = atoi(iso + 14);
  tmv.tm_sec  = 0;
  tmv.tm_isdst = -1;
  // mktime treats the broken-down time as local — exactly what we want
  // because open-meteo gave it in our timezone.
  time_t t = mktime(&tmv);
  return (t < 0) ? 0 : (uint32_t)t;
}

bool fetchWeather() {
  if (!g_loc.valid) return false;
  char url[320];
  snprintf(url, sizeof(url),
    "http://api.open-meteo.com/v1/forecast"
    "?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,weather_code,wind_speed_10m,"
    "relative_humidity_2m,is_day"
    "&daily=sunrise,sunset,precipitation_sum"
    "&timezone=%s&forecast_days=1",
    g_loc.lat, g_loc.lon, g_loc.timezone);
  // Spaces / special chars in timezone would need URL-encoding, but
  // IANA TZ names use only ASCII letters and "/" which open-meteo accepts.

  JsonDocument doc;
  if (!httpGetJson(url, doc, 5000)) return false;

  JsonVariantConst cur = doc["current"];
  if (cur.isNull()) {
    Serial.println(F("[world] open-meteo: no 'current' field"));
    return false;
  }
  g_weather.valid           = true;
  g_weather.temperatureC    = (int8_t)((float)(cur["temperature_2m"] | 0.0f));
  g_weather.weatherCode     = cur["weather_code"]         | 0;
  g_weather.windKmh         = (uint8_t)((float)(cur["wind_speed_10m"] | 0.0f));
  g_weather.humidityPct     = cur["relative_humidity_2m"] | 0;
  g_weather.isDay           = cur["is_day"]               | 0;

  JsonVariantConst daily = doc["daily"];
  const char* sr = daily["sunrise"][0] | "";
  const char* ss = daily["sunset"][0]  | "";
  g_weather.sunriseUtc = parseIso8601LocalAsEpoch(sr);
  g_weather.sunsetUtc  = parseIso8601LocalAsEpoch(ss);
  float precip = daily["precipitation_sum"][0] | 0.0f;
  if (precip < 0)    precip = 0;
  if (precip > 25.5) precip = 25.5;
  g_weather.precipMmTimes10 = (uint8_t)(precip * 10.0f);
  Serial.printf("[world] weather: code=%u temp=%d°C day=%u sunrise=%lu "
                "sunset=%lu precip=%.1fmm\n",
                g_weather.weatherCode, (int)g_weather.temperatureC,
                g_weather.isDay,
                (unsigned long)g_weather.sunriseUtc,
                (unsigned long)g_weather.sunsetUtc,
                precip);
  return true;
}

}  // namespace

// ─── Public API ────────────────────────────────────────────────────────

void loadWorldCache() {
  Preferences p;
  if (!p.begin("world", true)) {
    g_loc.valid = false;
    g_weather.valid = false;
    return;
  }
  g_loc.valid              = p.getBool("locVal", false);
  String city = p.getString("locCity", "");
  strncpy(g_loc.city, city.c_str(), sizeof(g_loc.city) - 1);
  g_loc.city[sizeof(g_loc.city) - 1] = '\0';
  String cc = p.getString("locCC", "");
  strncpy(g_loc.countryCode, cc.c_str(), sizeof(g_loc.countryCode) - 1);
  g_loc.countryCode[sizeof(g_loc.countryCode) - 1] = '\0';
  g_loc.lat                = p.getFloat ("locLat", 0);
  g_loc.lon                = p.getFloat ("locLon", 0);
  String tz = p.getString("locTz", "");
  strncpy(g_loc.timezone, tz.c_str(), sizeof(g_loc.timezone) - 1);
  g_loc.timezone[sizeof(g_loc.timezone) - 1] = '\0';
  g_loc.lastUpdatedEpoch   = p.getUInt  ("locUpd", 0);

  g_weather.valid          = p.getBool  ("wxVal",  false);
  g_weather.weatherCode    = p.getUShort("wxCode", 0);
  g_weather.temperatureC   = p.getChar  ("wxTemp", 0);
  g_weather.humidityPct    = p.getUChar ("wxHum",  0);
  g_weather.isDay          = p.getUChar ("wxDay",  1);
  g_weather.windKmh        = p.getUChar ("wxWind", 0);
  g_weather.precipMmTimes10= p.getUChar ("wxPrcp", 0);
  g_weather.sunriseUtc     = p.getUInt  ("wxSr",   0);
  g_weather.sunsetUtc      = p.getUInt  ("wxSs",   0);
  g_weather.lastUpdatedEpoch = p.getUInt("wxUpd",  0);
  p.end();

  Serial.printf("[world] cache loaded — locValid=%d wxValid=%d\n",
                g_loc.valid, g_weather.valid);
  if (g_loc.valid) {
    Serial.printf("[world]   loc: %s, %s (%.3f, %.3f) tz=%s upd=%lu\n",
                  g_loc.city, g_loc.countryCode, g_loc.lat, g_loc.lon,
                  g_loc.timezone, (unsigned long)g_loc.lastUpdatedEpoch);
  }
  if (g_weather.valid) {
    Serial.printf("[world]   wx : code=%u temp=%d°C hum=%u%% day=%u "
                  "wind=%u prcp=%u/10 sr=%lu ss=%lu upd=%lu\n",
                  g_weather.weatherCode,
                  (int)g_weather.temperatureC,
                  g_weather.humidityPct,
                  g_weather.isDay,
                  g_weather.windKmh,
                  g_weather.precipMmTimes10,
                  (unsigned long)g_weather.sunriseUtc,
                  (unsigned long)g_weather.sunsetUtc,
                  (unsigned long)g_weather.lastUpdatedEpoch);
  }
}

bool fetchWorldIfStale(uint32_t nowEpoch) {
  // Time-inversion: nowEpoch lies BEFORE the last cached update —
  // typical when the RTC has fallen back to the hardcoded fallback
  // (May 2026 instead of today) but the cache stems from a real NTP
  // sync. Logically that means: we don't know if the cache is fresh,
  // so refresh to be safe.
  bool locInversion = (g_loc.lastUpdatedEpoch != 0 &&
                       nowEpoch != 0 &&
                       nowEpoch < g_loc.lastUpdatedEpoch);
  bool wxInversion  = (g_weather.lastUpdatedEpoch != 0 &&
                       nowEpoch != 0 &&
                       nowEpoch < g_weather.lastUpdatedEpoch);
  bool needLoc = !g_loc.valid ||
                 nowEpoch == 0 ||
                 g_loc.lastUpdatedEpoch == 0 ||
                 locInversion ||
                 (nowEpoch - g_loc.lastUpdatedEpoch >= kLocationMaxAgeSec);
  bool needWx  = !g_weather.valid ||
                 g_weather.lastUpdatedEpoch == 0 ||
                 wxInversion ||
                 (nowEpoch > g_weather.lastUpdatedEpoch &&
                  nowEpoch - g_weather.lastUpdatedEpoch >= kWeatherMaxAgeSec);

  Serial.printf("[world] fetchIfStale: needLoc=%d needWx=%d "
                "(now=%lu locUpd=%lu wxUpd=%lu locInv=%d wxInv=%d)\n",
                needLoc, needWx,
                (unsigned long)nowEpoch,
                (unsigned long)g_loc.lastUpdatedEpoch,
                (unsigned long)g_weather.lastUpdatedEpoch,
                locInversion, wxInversion);

  if (needLoc) {
    if (fetchLocation()) {
      g_loc.lastUpdatedEpoch = nowEpoch;
      saveLocation();
      g_freshThisSession = true;
    }
  }
  if (needWx && g_loc.valid) {
    if (fetchWeather()) {
      g_weather.lastUpdatedEpoch = nowEpoch;
      saveWeather();
      g_freshThisSession = true;
    }
  }

  // Always recompute moon — it's just math, costs nothing.
  calcMoon(nowEpoch);
  return g_freshThisSession;
}

void invalidateLocationCache() {
  g_loc.lastUpdatedEpoch = 0;
}

const WorldLocation& worldLocation() { return g_loc; }
const WorldWeather&  worldWeather()  { return g_weather; }
const WorldMoon&     worldMoon()     { return g_moon; }
bool worldHasFreshData() { return g_freshThisSession; }

void recomputeMoonPhase(uint32_t nowEpoch) { calcMoon(nowEpoch); }
