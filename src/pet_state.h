#pragma once
#include <Arduino.h>
#include <M5Unified.h>

// ─── Enums ───────────────────────────────────────────────────────────────────

enum class TimePhase : uint8_t { Morning, Day, Evening, Night };

enum class TouchZone : uint8_t {
  None, Forehead, LeftEye, RightEye, LeftCheek, RightCheek, Mouth,
  LeftEar, RightEar,
};

enum class MicroAnim : uint8_t { None, Sneeze, Hiccup, EarTwitch, LookAround };

enum class Bubble : uint8_t { None, Hungry, Sleepy, Loved, Question };

// ─── Needs ───────────────────────────────────────────────────────────────────
//
// Definition of `Needs`, `computeMood()` und `decayNeeds()` liegt in
// needs_logic.{h,cpp} — Arduino/M5-frei, damit es im Native-PIO-Env
// unit-getestet werden kann. Hier nur re-exportieren.

#include "needs_logic.h"

// ─── Time-of-day (RTC) ───────────────────────────────────────────────────────

struct WallDate { uint16_t year; uint8_t month; uint8_t day; };

bool        rtcAvailable();             // false until RTC has a sane year
TimePhase   currentPhase();              // Day if RTC unset
WallDate    currentDate();               // {0,0,0} if unset
int         daysBetween(const WallDate& a, const WallDate& b);

// ─── Persistence (NVS) ───────────────────────────────────────────────────────

struct Persisted {
  Needs needs;
  WallDate born;
  WallDate lastSeen;
  uint8_t  brightnessLevel;     // 0..3 (25/50/75/100 %)
  uint8_t  volume;               // 0..255
  uint8_t  animal;               // 0=Bear, 1=Cat, 2=Dog

  // Foraging inventory (food collected via the foraging screen). Each unit
  // is one feed; a feed action picks the first non-zero in apple→berry→fish
  // order. Capped at kFoodMaxPerType in main.cpp.
  uint16_t apples;
  uint16_t berries;
  uint16_t fish;

  // Selected play toy (index into Toy enum) + how many times in a row it
  // has been used. After ~3 repeats the pet starts to look bored, forcing
  // the user to switch toys via the toy-select screen.
  uint8_t  selectedToy;
  uint8_t  toyRepeatCount;

  // Selected travel destination (index into Scene enum). Drives the
  // background of the pet view. Persists so the pet wakes up wherever it
  // last was.
  uint8_t  selectedScene;

  // Mini-game high scores (one per scene's activity).
  uint16_t bestButterflies;     // Meadow
  uint16_t bestStack;           // Bedroom
  uint16_t bestMushrooms;       // Forest
  uint16_t bestSurf;            // Beach
  uint16_t bestScorpion;        // Desert
  uint16_t bestAsteroids;       // Space
  uint16_t bestCross;           // City

  // Mini-game cooldowns. Each entry stores the wall-clock epoch second
  // when the corresponding scene's activity becomes available again. 0 =
  // immediately available. Persisting these means the cooldown survives a
  // reboot — playing then powering off won't let the user reset it.
  // Indexed by Scene enum (0..6).
  uint32_t gameCooldownEnd[7];

  // Scene-change cooldown. Same wall-clock-epoch semantics as the game
  // cooldowns. Set whenever the user travels to a different scene; the
  // travel button is locked until this passes.
  uint32_t travelCooldownEnd;

  // Parental "play time" lockout — wall-clock epoch second until which
  // the device should refuse to start. Set when a 30-minute play session
  // ends and committed to NVS so a power-cycle can't reset it.
  uint32_t playLockoutEndSec;

  // UI language. languageChosen=0 triggers the first-run language + pet
  // picker after the splash; once chosen, languageChosen stays 1 forever.
  // language: 0 = DE, 1 = EN.
  uint8_t  language;
  uint8_t  languageChosen;

  // Configurable play-session limit (minutes). Replaces the previous
  // hardcoded 30. Written via the parent web server. Default at first
  // boot: 30. Range clamped to 5..120 by callers.
  uint8_t  sessionLimitMin;

  // Total minutes the pet has been actively used. Persisted; bumped
  // every minute the device is awake on the pet view (not while on the
  // splash, lockout, etc.). Wraps at ~70 years, so effectively forever.
  uint32_t totalPlayMin;

  // Sport-mode tracking. Streak counts consecutive days with at least one
  // completed workout; gets reset if a calendar day is missed. totalReps
  // accumulates lifetime exercise reps. lastDay = day of last completed
  // workout (year/month/day; 0/0/0 = never).
  WallDate sportLastDay;
  uint16_t sportStreakDays;
  uint32_t sportTotalReps;

  // Last calendar day NTP sync succeeded — boot skips the WiFi cycle
  // when today already matches (RTC is battery-backed and accurate to
  // a few seconds per day).
  WallDate lastNtpDay;

  // Pip companion mode: when true, the pet keeps an ESP-NOW listener
  // running in the background to receive packets from a paired Pip
  // (treats, step reports, wand gestures, …). Default off — opt-in via
  // settings, since the always-on radio shaves ~20 % off the battery
  // runtime. See pip_link.{h,cpp} for the listener.
  uint8_t  pipMode;
};

bool loadPersisted(Persisted& out);
void savePersisted(const Persisted& p);
void resetNeedsPersisted(Persisted& p);  // resets needs to 100/100/100
void clearPersisted();                    // wipes the entire NVS namespace

// ─── Gaze ────────────────────────────────────────────────────────────────────

struct Gaze {
  float currentX = 0, currentY = 0;     // smoothed (-4..+4 pixels)
  float targetX  = 0, targetY  = 0;     // requested target
  uint32_t nextSaccadeMs = 0;
};

void updateGaze(Gaze& g, bool touched, int touchX, int touchY,
                uint32_t now_ms);

// ─── Tilt ────────────────────────────────────────────────────────────────────

struct Tilt {
  float offsetX = 0, offsetY = 0;       // smoothed pixel offset (-5..+5)
};

void updateTilt(Tilt& t, float gx, float gy);

// ─── Wander (slow drift in tilt direction) ──────────────────────────────────

struct Wander {
  float x = 0, y = 0;                   // accumulated px offset
  uint32_t lastUpdateMs = 0;

  // Swing/back-and-forth detection — used to fire a "fun" event when the
  // pet has been rocked left↔right repeatedly.
  float    lastCheckX = 0;
  uint32_t lastCheckMs = 0;
  int8_t   lastDirX = 0;          // +1 right, -1 left, 0 unknown
  uint32_t firstSwingMs = 0;
  uint8_t  swingCount = 0;
  uint32_t lastFunEventMs = 0;

  // Lullaby tracking — total swings within a longer rolling window. Once
  // enough sustained rocking accumulates, the pet gets sleepy.
  uint8_t  lullabyTotal = 0;
  uint32_t lullabyFirstSwingMs = 0;
  uint32_t lastLullabyMs = 0;
};

// Drifts the pet's position toward the way the device is tilted. While
// `sleeping`, drift is frozen and no fun/lullaby events fire.
//   funEvent     — set true on the frame the pet detects "back-and-forth"
//                  rocking (a few quick reversals).
//   lullabyEvent — set true on the frame sustained rocking has built up
//                  enough to put the pet to sleep.
void updateWander(Wander& w, float gx, float gy, uint32_t now_ms,
                  bool sleeping = false,
                  bool* funEvent = nullptr,
                  bool* lullabyEvent = nullptr);

// ─── Micro animations ────────────────────────────────────────────────────────

struct Micro {
  MicroAnim current = MicroAnim::None;
  uint32_t  startMs = 0;
  uint32_t  untilMs = 0;
  uint32_t  nextMs  = 0;
};

void updateMicro(Micro& m, uint32_t now_ms, bool eligible);

// ─── Thought bubbles ─────────────────────────────────────────────────────────

struct BubbleState {
  Bubble current = Bubble::None;
  uint32_t startMs = 0;
  uint32_t untilMs = 0;
  uint32_t nextMs  = 0;
};

// Decides which bubble (if any) should appear. `recentlyPetted` = at least
// 3 stroke events in the last 5 s. `justWokeUp` = pet transitioned out of
// Sleeping in the last 1 s.
void updateBubble(BubbleState& b, uint32_t now_ms, const Needs& n,
                  bool recentlyPetted, bool justWokeUp);

// ─── Floating action icons ──────────────────────────────────────────────────

enum class FloatType : uint8_t { None, Heart, Apple, Zzz, Ball, Note };

struct FloatingIcon {
  FloatType type    = FloatType::None;
  int16_t   fromX   = 0, fromY = 0;
  int16_t   toX     = 0, toY   = 0;
  uint32_t  startMs = 0;
  uint32_t  durMs   = 0;
};

struct FloatPool {
  static constexpr uint8_t N = 6;
  FloatingIcon items[N];
};

// Spawn one icon flying from (fx,fy) to (tx,ty). Recycles the oldest slot
// if the pool is full. `durMs` controls how long the icon is in flight —
// short for "lands on a bar" feedback, longer for things like a ball
// flying across the screen.
void spawnFloat(FloatPool& p, FloatType t,
                int fromX, int fromY, int toX, int toY,
                uint32_t now_ms, uint32_t durMs = 450);

// ─── Touch-zone classification ──────────────────────────────────────────────

// Returns which face area the (x,y) point hits. `volumeSliderVisible` and
// the button column area are excluded from the head test so UI taps don't
// also trigger a face-zone reaction.
TouchZone classifyTouchZone(int x, int y, bool volumeSliderVisible,
                            int headOffsetX = 0, int headOffsetY = 0);
