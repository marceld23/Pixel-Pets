#pragma once
#include <Arduino.h>

// ─── World awareness module ────────────────────────────────────────────────
//
// At boot (after NTP), the pet briefly fetches:
//   • approximate location  → ip-api.com         (city, lat, lon, timezone)
//   • weather + sun         → api.open-meteo.com (current weather, sunrise,
//                                                 sunset, daylight, precip)
//   • moon phase            → calculated locally (no network needed)
// Results live in NVS namespace "world" with caching:
//   • location :  7 days
//   • weather  :  1 hour
//
// derived helpers translate these into a small WorldState used by the
// rest of the firmware (TimePhase from real sunrise/sunset, weather
// override for the bedroom window etc.).

struct WorldLocation {
  bool     valid;
  char     city[40];
  char     countryCode[4];
  float    lat;
  float    lon;
  char     timezone[40];
  uint32_t lastUpdatedEpoch;     // unix seconds; 0 = never
};

struct WorldWeather {
  bool     valid;
  uint16_t weatherCode;          // open-meteo WMO code
  int8_t   temperatureC;
  uint8_t  humidityPct;
  uint8_t  isDay;                // 0/1
  uint8_t  windKmh;
  uint8_t  precipMmTimes10;      // mm × 10 to avoid floats
  uint32_t sunriseUtc;
  uint32_t sunsetUtc;
  uint32_t lastUpdatedEpoch;
};

enum class MoonPhase : uint8_t {
  NewMoon = 0,
  WaxingCrescent,
  FirstQuarter,
  WaxingGibbous,
  FullMoon,
  WaningGibbous,
  LastQuarter,
  WaningCrescent,
};

struct WorldMoon {
  MoonPhase phase;
  float     phase01;             // 0..1 along the synodic cycle
  uint8_t   illuminationPct;     // approximate, 0..100
};

// Maps the open-meteo WMO weather code into the existing per-scene Weather
// enum (Sunny / Cloudy / Rainy / Foggy / Sandstorm). Snow and storms map
// to the closest existing visual.
//   Forward-declared here; the tiny mapping helper lives in world.cpp.

// ─── Loading / fetching ─────────────────────────────────────────────────
// loadWorldCache() pulls the cached values out of NVS into the globals.
// Call once at boot. fetchWorldIfStale() does the HTTP work after WiFi is
// up; caller hands over a connected-WiFi window.
void loadWorldCache();
bool fetchWorldIfStale(uint32_t nowEpoch);

// Force a re-fetch on next call (used by Settings → "Standort neu
// ermitteln"). Doesn't clear the saved values, just marks them stale.
void invalidateLocationCache();

// ─── Snapshot accessors ────────────────────────────────────────────────
const WorldLocation& worldLocation();
const WorldWeather&  worldWeather();
const WorldMoon&     worldMoon();
bool                 worldHasFreshData();   // true once at least one fetch succeeded this session

// ─── Derived helpers ────────────────────────────────────────────────────
// Recomputes the moon-phase struct based on the current Unix epoch.
// Cheap — call any time the date changes.
void recomputeMoonPhase(uint32_t nowEpoch);
