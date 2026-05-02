#include "needs_logic.h"

uint8_t computeMood(const Needs& n) {
  // Mood = happiness, but pulled down by a critical secondary need.
  //
  // - Above 30, energy and fullness don't penalise mood at all — happiness
  //   alone determines high moods (this lets `Love` actually be reachable).
  // - Below 30, they ramp linearly to zero, so a starving or exhausted pet
  //   really does become Sad regardless of happiness.
  uint16_t lo = n.happiness;
  if (n.energy < 30) {
    uint16_t e = (uint16_t)n.energy * 100 / 30;
    if (e < lo) lo = e;
  }
  if (n.fullness < 30) {
    uint16_t f = (uint16_t)n.fullness * 100 / 30;
    if (f < lo) lo = f;
  }
  return (uint8_t)lo;
}

void decayNeeds(Needs& n, uint32_t now_ms, uint32_t& last_decay_ms, bool sleeping) {
  // We tick on a 1-second granularity so decay rates are stable.
  if (last_decay_ms == 0) last_decay_ms = now_ms;
  uint32_t elapsed = now_ms - last_decay_ms;
  if (elapsed < 1000) return;
  uint32_t ticks = elapsed / 1000;
  last_decay_ms += ticks * 1000;

  for (uint32_t t = 0; t < ticks; ++t) {
    static uint32_t happCounter = 0;
    static uint32_t engCounter  = 0;
    static uint32_t fullCounter = 0;

    if (sleeping) {
      // While sleeping the pet only regenerates energy. Happiness and
      // fullness are frozen (their counters keep their position so decay
      // resumes on the same schedule when the pet wakes).
      if (++engCounter >= 2) { engCounter = 0;
        if (n.energy < 100) n.energy++;     // +1 per 2s → 0..100 in ~3.3 min
      }
    } else {
      if (++happCounter >= 5)  { happCounter = 0;
        if (n.happiness > 0) n.happiness--;
      }
      if (++engCounter >= 10)  { engCounter = 0;
        if (n.energy > 0) n.energy--;
      }
      if (++fullCounter >= 12) { fullCounter = 0;
        if (n.fullness > 0) n.fullness--;
      }
    }
  }
}
