#include "pet_state.h"
#include "world.h"
#include <Preferences.h>
#include <math.h>
#include <time.h>

// Face geometry constants must match face.cpp's layout. Kept here for the
// touch-zone classifier; if those change in face.cpp, update both.
static constexpr int CANVAS_W = 320;
static constexpr int CANVAS_H = 240;
static constexpr int CX = CANVAS_W / 2;
static constexpr int CY = CANVAS_H / 2 + 8;
static constexpr int HEAD_RX = 110;
static constexpr int HEAD_RY = 92;
static constexpr int EYE_DX  = 38;
static constexpr int EYE_OFF_Y = -18;
static constexpr int MOUTH_OFF_Y = 32;

// computeMood / decayNeeds wurden in needs_logic.{h,cpp} verschoben — reine
// Logik, Arduino-frei, in der Native-Test-Env unter test/ unit-getestet.

// ─── Time-of-day ─────────────────────────────────────────────────────────────

bool rtcAvailable() {
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt)) return false;
  return dt.date.year >= 2024;
}

TimePhase currentPhase() {
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt) || dt.date.year < 2024) return TimePhase::Day;

  int nowMin = (int)dt.time.hours * 60 + (int)dt.time.minutes;

  // Prefer the cached real sunrise/sunset from open-meteo if we have it.
  const WorldWeather& w = worldWeather();
  if (w.valid && w.sunriseUtc != 0 && w.sunsetUtc != 0) {
    struct tm tm_sr, tm_ss;
    time_t sr = (time_t)w.sunriseUtc;
    time_t ss = (time_t)w.sunsetUtc;
    if (localtime_r(&sr, &tm_sr) && localtime_r(&ss, &tm_ss)) {
      int srMin = tm_sr.tm_hour * 60 + tm_sr.tm_min;
      int ssMin = tm_ss.tm_hour * 60 + tm_ss.tm_min;
      // Phase windows around the real horizon crossings:
      //   Night    : > 30 min before sunrise OR > 15 min after sunset
      //   Morning  : 30 min before — 60 min after sunrise
      //   Day      : 60 min after sunrise — 60 min before sunset
      //   Evening  : 60 min before — 15 min after sunset
      if (nowMin < srMin - 30)   return TimePhase::Night;
      if (nowMin < srMin + 60)   return TimePhase::Morning;
      if (nowMin < ssMin - 60)   return TimePhase::Day;
      if (nowMin < ssMin + 15)   return TimePhase::Evening;
      return TimePhase::Night;
    }
  }

  // Fallback heuristic (no world data yet).
  uint8_t h = dt.time.hours;
  if (h < 6)  return TimePhase::Night;
  if (h < 9)  return TimePhase::Morning;
  if (h < 18) return TimePhase::Day;
  if (h < 22) return TimePhase::Evening;
  return TimePhase::Night;
}

WallDate currentDate() {
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt) || dt.date.year < 2024) return WallDate{0, 0, 0};
  return WallDate{(uint16_t)dt.date.year,
                  (uint8_t) dt.date.month,
                  (uint8_t) dt.date.date};
}

// Approximation: assumes 30-day months. Good enough for "you've been gone
// for ~N days" greetings.
int daysBetween(const WallDate& a, const WallDate& b) {
  if (a.year == 0 || b.year == 0) return 0;
  int da = a.year * 365 + a.month * 30 + a.day;
  int db = b.year * 365 + b.month * 30 + b.day;
  return db - da;
}

// ─── Persistence ─────────────────────────────────────────────────────────────

namespace { Preferences nvs; }

bool loadPersisted(Persisted& out) {
  if (!nvs.begin("pet", true)) return false;
  bool ok = nvs.isKey("hap");
  if (ok) {
    out.needs.happiness = nvs.getUChar("hap", 60);
    out.needs.energy    = nvs.getUChar("eng", 100);
    out.needs.fullness  = nvs.getUChar("ful", 100);
    out.born.year       = nvs.getUShort("bornY", 0);
    out.born.month      = nvs.getUChar ("bornM", 0);
    out.born.day        = nvs.getUChar ("bornD", 0);
    out.lastSeen.year   = nvs.getUShort("seenY", 0);
    out.lastSeen.month  = nvs.getUChar ("seenM", 0);
    out.lastSeen.day    = nvs.getUChar ("seenD", 0);
    out.brightnessLevel = nvs.getUChar ("bri", 3);   // default 100 %
    out.volume          = nvs.getUChar ("vol", 51);  // default 20 %
    out.animal          = nvs.getUChar ("ani", 0);   // default Bear
    out.apples          = nvs.getUShort("apl", 3);   // start with a few apples
    out.berries         = nvs.getUShort("ber", 0);
    out.fish            = nvs.getUShort("fsh", 0);
    out.selectedToy     = nvs.getUChar ("toy", 0);   // default Ball
    out.toyRepeatCount  = nvs.getUChar ("trp", 0);
    out.selectedScene   = nvs.getUChar ("scn", 0);   // default Meadow
    out.bestButterflies = nvs.getUShort("bbf", 0);
    out.bestStack       = nvs.getUShort("bst", 0);
    out.bestMushrooms   = nvs.getUShort("bms", 0);
    out.bestSurf        = nvs.getUShort("bsr", 0);
    out.bestScorpion    = nvs.getUShort("bsk", 0);
    out.bestAsteroids   = nvs.getUShort("bas", 0);
    out.bestCross       = nvs.getUShort("bcr", 0);
    out.gameCooldownEnd[0] = nvs.getUInt("cd0", 0);
    out.gameCooldownEnd[1] = nvs.getUInt("cd1", 0);
    out.gameCooldownEnd[2] = nvs.getUInt("cd2", 0);
    out.gameCooldownEnd[3] = nvs.getUInt("cd3", 0);
    out.gameCooldownEnd[4] = nvs.getUInt("cd4", 0);
    out.gameCooldownEnd[5] = nvs.getUInt("cd5", 0);
    out.gameCooldownEnd[6] = nvs.getUInt("cd6", 0);
    out.travelCooldownEnd  = nvs.getUInt("tcd", 0);
    out.playLockoutEndSec  = nvs.getUInt("lck", 0);
    out.language           = nvs.getUChar("lng", 0);
    out.languageChosen     = nvs.getUChar("lcs", 0);
    out.sessionLimitMin    = nvs.getUChar("slim", 30);
    out.totalPlayMin       = nvs.getUInt ("tpm",  0);
    out.sportLastDay.year  = nvs.getUShort("spLY", 0);
    out.sportLastDay.month = nvs.getUChar ("spLM", 0);
    out.sportLastDay.day   = nvs.getUChar ("spLD", 0);
    out.sportStreakDays    = nvs.getUShort("spStr", 0);
    out.sportTotalReps     = nvs.getUInt ("spRep", 0);
    out.lastNtpDay.year    = nvs.getUShort("ntpY", 0);
    out.lastNtpDay.month   = nvs.getUChar ("ntpM", 0);
    out.lastNtpDay.day     = nvs.getUChar ("ntpD", 0);
    out.pipMode            = nvs.getUChar ("pip",  0);   // default off
  }
  nvs.end();
  return ok;
}

void savePersisted(const Persisted& p) {
  if (!nvs.begin("pet", false)) return;
  nvs.putUChar ("hap",   p.needs.happiness);
  nvs.putUChar ("eng",   p.needs.energy);
  nvs.putUChar ("ful",   p.needs.fullness);
  nvs.putUShort("bornY", p.born.year);
  nvs.putUChar ("bornM", p.born.month);
  nvs.putUChar ("bornD", p.born.day);
  nvs.putUShort("seenY", p.lastSeen.year);
  nvs.putUChar ("seenM", p.lastSeen.month);
  nvs.putUChar ("seenD", p.lastSeen.day);
  nvs.putUChar ("bri",   p.brightnessLevel);
  nvs.putUChar ("vol",   p.volume);
  nvs.putUChar ("ani",   p.animal);
  nvs.putUShort("apl",   p.apples);
  nvs.putUShort("ber",   p.berries);
  nvs.putUShort("fsh",   p.fish);
  nvs.putUChar ("toy",   p.selectedToy);
  nvs.putUChar ("trp",   p.toyRepeatCount);
  nvs.putUChar ("scn",   p.selectedScene);
  nvs.putUShort("bbf",   p.bestButterflies);
  nvs.putUShort("bst",   p.bestStack);
  nvs.putUShort("bms",   p.bestMushrooms);
  nvs.putUShort("bsr",   p.bestSurf);
  nvs.putUShort("bsk",   p.bestScorpion);
  nvs.putUShort("bas",   p.bestAsteroids);
  nvs.putUShort("bcr",   p.bestCross);
  nvs.putUInt  ("cd0",   p.gameCooldownEnd[0]);
  nvs.putUInt  ("cd1",   p.gameCooldownEnd[1]);
  nvs.putUInt  ("cd2",   p.gameCooldownEnd[2]);
  nvs.putUInt  ("cd3",   p.gameCooldownEnd[3]);
  nvs.putUInt  ("cd4",   p.gameCooldownEnd[4]);
  nvs.putUInt  ("cd5",   p.gameCooldownEnd[5]);
  nvs.putUInt  ("cd6",   p.gameCooldownEnd[6]);
  nvs.putUInt  ("tcd",   p.travelCooldownEnd);
  nvs.putUInt  ("lck",   p.playLockoutEndSec);
  nvs.putUChar ("lng",   p.language);
  nvs.putUChar ("lcs",   p.languageChosen);
  nvs.putUChar ("slim",  p.sessionLimitMin);
  nvs.putUInt  ("tpm",   p.totalPlayMin);
  nvs.putUShort("spLY",  p.sportLastDay.year);
  nvs.putUChar ("spLM",  p.sportLastDay.month);
  nvs.putUChar ("spLD",  p.sportLastDay.day);
  nvs.putUShort("spStr", p.sportStreakDays);
  nvs.putUInt  ("spRep", p.sportTotalReps);
  nvs.putUShort("ntpY",  p.lastNtpDay.year);
  nvs.putUChar ("ntpM",  p.lastNtpDay.month);
  nvs.putUChar ("ntpD",  p.lastNtpDay.day);
  nvs.putUChar ("pip",   p.pipMode);
  nvs.end();
}

void resetNeedsPersisted(Persisted& p) {
  p.needs.happiness = 100;
  p.needs.energy    = 100;
  p.needs.fullness  = 100;
}

// Wipes every key in the "pet" NVS namespace — full factory reset. The
// next loadPersisted() will return false and the setup() defaults branch
// runs, including the first-run language + pet picker.
void clearPersisted() {
  if (!nvs.begin("pet", false)) return;
  nvs.clear();
  nvs.end();
}

// ─── Gaze ────────────────────────────────────────────────────────────────────

void updateGaze(Gaze& g, bool touched, int touchX, int touchY, uint32_t now_ms) {
  // Eye centers in canvas coords (left/right are symmetrical, use mid-point)
  const float eyeMidX = (float)CX;
  const float eyeMidY = (float)(CY + EYE_OFF_Y);

  if (touched) {
    // Aim pupil toward touch point, but clamp magnitude so the pupil stays
    // inside the eye.
    float dx = (float)touchX - eyeMidX;
    float dy = (float)touchY - eyeMidY;
    float len = sqrtf(dx*dx + dy*dy);
    const float maxOff = 4.0f;
    const float reach = 100.0f;  // distance at which gaze hits its limit
    float scale = (len < 1.0f) ? 0.0f : fminf(len, reach) / reach * maxOff / fmaxf(1.0f, len);
    g.targetX = dx * scale;
    g.targetY = dy * scale;
    // Reset saccade timer so we don't immediately drift away after release.
    g.nextSaccadeMs = now_ms + 1000 + (now_ms % 2000);
  } else if (now_ms >= g.nextSaccadeMs) {
    // Pick a fresh idle target.
    g.targetX = ((int32_t)(now_ms ^ 0x9E37) % 9) - 4;       // -4..+4
    g.targetY = ((int32_t)(now_ms ^ 0xCD3F) % 7) - 3;       // -3..+3
    g.nextSaccadeMs = now_ms + 2000 + ((now_ms ^ 0xA37) % 3000);
  }

  // Smooth interpolation toward target.
  const float lerp = 0.18f;
  g.currentX += (g.targetX - g.currentX) * lerp;
  g.currentY += (g.targetY - g.currentY) * lerp;
}

// ─── Tilt ────────────────────────────────────────────────────────────────────

void updateWander(Wander& w, float gx, float gy, uint32_t now_ms,
                  bool sleeping, bool* funEvent, bool* lullabyEvent) {
  if (funEvent)     *funEvent     = false;
  if (lullabyEvent) *lullabyEvent = false;

  if (w.lastUpdateMs == 0) { w.lastUpdateMs = now_ms; return; }
  float dt = (now_ms - w.lastUpdateMs) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;
  w.lastUpdateMs = now_ms;

  if (sleeping) {
    // Sleeping pet doesn't slide and has no fun/lullaby reaction.
    w.swingCount    = 0;
    w.lastDirX      = 0;
    w.lastCheckMs   = 0;
    w.lullabyTotal  = 0;
    return;
  }

  // ── Drift ──
  float dirX = -gx;
  float dirY =  gy;

  constexpr float DEAD_ZONE = 0.10f;
  constexpr float SPEED_PX  = 150.0f;    // px / second at 1g lateral tilt
  float mag = sqrtf(dirX * dirX + dirY * dirY);
  if (mag > DEAD_ZONE) {
    float scale = (mag - DEAD_ZONE) / mag;
    w.x += dirX * scale * SPEED_PX * dt;
    w.y += dirY * scale * SPEED_PX * dt;
  }
  constexpr float X_RANGE = 50.0f;
  constexpr float Y_UP    = 20.0f;
  constexpr float Y_DOWN  = 10.0f;   // leave room for the button-hint strip
  if (w.x < -X_RANGE) w.x = -X_RANGE;
  if (w.x >  X_RANGE) w.x =  X_RANGE;
  if (w.y < -Y_UP)    w.y = -Y_UP;
  if (w.y >  Y_DOWN)  w.y =  Y_DOWN;

  // ── Swing detection ──
  // Look directly at the (smoothed) gravity-direction signal, sampled every
  // 200 ms with a hysteresis band around 0. Using gx itself (not the slow
  // wander.x integral) means a 1-Hz rocking gesture cleanly produces sign
  // changes — accumulating wander.x dx over 200 ms is far too small for
  // typical rocking frequencies.
  if (w.lastCheckMs == 0) {
    w.lastCheckMs = now_ms;
    w.lastCheckX  = w.x;       // unused by the new detector but kept for compat
  }
  if (now_ms - w.lastCheckMs >= 200) {
    w.lastCheckX  = w.x;
    w.lastCheckMs = now_ms;

    // -gx is the "screen-X tilt direction" used everywhere else.
    float tilt = -gx;
    int8_t dir = (tilt > 0.10f) ? 1 : (tilt < -0.10f) ? -1 : 0;
    if (dir != 0 && dir != w.lastDirX) {
      if (w.lastDirX != 0) {
        // Direction reversal
        if (w.swingCount == 0 || now_ms - w.firstSwingMs > 3000) {
          w.firstSwingMs = now_ms;
          w.swingCount   = 1;
        } else {
          w.swingCount++;
        }
        // Independent lullaby tally over a longer window
        if (w.lullabyTotal == 0 || now_ms - w.lullabyFirstSwingMs > 10000) {
          w.lullabyFirstSwingMs = now_ms;
          w.lullabyTotal = 1;
        } else if (w.lullabyTotal < 255) {
          w.lullabyTotal++;
        }
      }
      w.lastDirX = dir;
    }

    if (w.swingCount > 0 && now_ms - w.firstSwingMs > 3000) {
      w.swingCount = 0;
      w.lastDirX   = 0;
    }
    if (w.lullabyTotal > 0 && now_ms - w.lullabyFirstSwingMs > 10000) {
      w.lullabyTotal = 0;
    }

    // Lullaby fires first — sustained rocking puts the pet to sleep. We
    // skip the fun event on the same frame so the user gets a clean
    // "drowsy" reaction instead of a celebration overlap.
    bool cooledDown = (w.lastLullabyMs == 0) ||
                      (now_ms - w.lastLullabyMs > 15000);
    if (w.lullabyTotal >= 6 && cooledDown) {
      w.lastLullabyMs   = now_ms;
      w.lullabyTotal    = 0;
      w.swingCount      = 0;
      w.lastDirX        = 0;
      if (lullabyEvent) *lullabyEvent = true;
    } else if (w.swingCount >= 2 && now_ms - w.lastFunEventMs > 1500) {
      w.lastFunEventMs = now_ms;
      w.swingCount = 0;
      w.lastDirX   = 0;
      if (funEvent) *funEvent = true;
    }
  }
}

void updateTilt(Tilt& t, float gx, float gy) {
  // gravity vector x≈0 when flat; positive x = tilted right, etc.
  float targetX = -gx * 5.0f;     // invert so head leans the way you tilt
  float targetY =  gy * 3.0f;
  if (targetX > 5)  targetX = 5;   if (targetX < -5) targetX = -5;
  if (targetY > 4)  targetY = 4;   if (targetY < -4) targetY = -4;
  const float lerp = 0.10f;
  t.offsetX += (targetX - t.offsetX) * lerp;
  t.offsetY += (targetY - t.offsetY) * lerp;
}

// ─── Micro animations ────────────────────────────────────────────────────────

void updateMicro(Micro& m, uint32_t now_ms, bool eligible) {
  if (m.current != MicroAnim::None) {
    if (now_ms >= m.untilMs) {
      m.current = MicroAnim::None;
      m.nextMs = now_ms + 8000 + ((now_ms ^ 0x5A5A) % 17000);  // 8..25 s
    }
    return;
  }
  if (!eligible || now_ms < m.nextMs) return;

  uint32_t pick = (now_ms ^ 0xBADC0DE) % 4;
  MicroAnim a = MicroAnim::None;
  uint32_t  d = 500;
  switch (pick) {
    case 0: a = MicroAnim::Sneeze;     d = 500;  break;
    case 1: a = MicroAnim::Hiccup;     d = 300;  break;
    case 2: a = MicroAnim::EarTwitch;  d = 250;  break;
    case 3: a = MicroAnim::LookAround; d = 1500; break;
  }
  m.current = a;
  m.startMs = now_ms;
  m.untilMs = now_ms + d;
}

// ─── Thought bubbles ────────────────────────────────────────────────────────

void updateBubble(BubbleState& b, uint32_t now_ms, const Needs& n,
                  bool recentlyPetted, bool justWokeUp) {
  if (b.current != Bubble::None) {
    if (now_ms >= b.untilMs) {
      b.current = Bubble::None;
      b.nextMs = now_ms + 15000;
    }
    return;
  }
  if (now_ms < b.nextMs) return;

  Bubble pick = Bubble::None;
  if      (justWokeUp)        pick = Bubble::Question;
  else if (n.fullness < 25)   pick = Bubble::Hungry;
  else if (n.energy   < 25)   pick = Bubble::Sleepy;
  else if (recentlyPetted &&
           n.happiness > 75)  pick = Bubble::Loved;

  if (pick == Bubble::None) return;

  b.current = pick;
  b.startMs = now_ms;
  b.untilMs = now_ms + 3000;
}

// ─── Floating action icons ──────────────────────────────────────────────────

void spawnFloat(FloatPool& p, FloatType t,
                int fx, int fy, int tx, int ty, uint32_t now,
                uint32_t durMs) {
  // Find a free slot first (None or already expired).
  int slot = -1;
  for (int i = 0; i < FloatPool::N; ++i) {
    if (p.items[i].type == FloatType::None ||
        now >= p.items[i].startMs + p.items[i].durMs) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    // All in flight — recycle the oldest one.
    uint32_t oldest = p.items[0].startMs;
    slot = 0;
    for (int i = 1; i < FloatPool::N; ++i) {
      if (p.items[i].startMs < oldest) {
        oldest = p.items[i].startMs;
        slot = i;
      }
    }
  }

  p.items[slot].type    = t;
  p.items[slot].fromX   = (int16_t)fx;
  p.items[slot].fromY   = (int16_t)fy;
  p.items[slot].toX     = (int16_t)tx;
  p.items[slot].toY     = (int16_t)ty;
  p.items[slot].startMs = now;
  p.items[slot].durMs   = durMs;
}

// ─── Touch zone classification ──────────────────────────────────────────────

TouchZone classifyTouchZone(int x, int y, bool volumeSliderVisible,
                            int headOffsetX, int headOffsetY) {
  if (x < 44) return TouchZone::None;
  if (volumeSliderVisible && x >= 280) return TouchZone::None;

  int hcx = CX + headOffsetX;
  int hcy = CY + headOffsetY;

  // Ears poke up above the head ellipse — must be checked BEFORE the
  // ellipse-bounds test, otherwise their visible top half rejects.
  // Centres match drawHead: (hx ± 70, hy - HEAD_RY + 6) with radius 15.
  int earY = hcy - HEAD_RY + 6;
  int leDx = x - (hcx - 70), leDy = y - earY;
  if (leDx*leDx + leDy*leDy < 15*15) return TouchZone::LeftEar;
  int reDx = x - (hcx + 70), reDy = y - earY;
  if (reDx*reDx + reDy*reDy < 15*15) return TouchZone::RightEar;

  float dx = (float)(x - hcx);
  float dy = (float)(y - hcy);
  float test = (dx*dx) / (float)(HEAD_RX*HEAD_RX) +
               (dy*dy) / (float)(HEAD_RY*HEAD_RY);
  if (test > 1.0f) return TouchZone::None;

  int leyeDx = x - (hcx - EYE_DX);
  int leyeDy = y - (hcy + EYE_OFF_Y);
  if (leyeDx*leyeDx + leyeDy*leyeDy < 18*18) return TouchZone::LeftEye;
  int reyeDx = x - (hcx + EYE_DX);
  int reyeDy = y - (hcy + EYE_OFF_Y);
  if (reyeDx*reyeDx + reyeDy*reyeDy < 18*18) return TouchZone::RightEye;

  if (y < hcy + EYE_OFF_Y - 5) return TouchZone::Forehead;

  if (abs(x - hcx) < 32 && abs(y - (hcy + MOUTH_OFF_Y)) < 18) return TouchZone::Mouth;

  if (y > hcy + EYE_OFF_Y + 8 && y < hcy + MOUTH_OFF_Y - 6) {
    return (x < hcx) ? TouchZone::LeftCheek : TouchZone::RightCheek;
  }

  return TouchZone::Forehead;
}
