#pragma once
#include <Arduino.h>

// ─── Pip → bigger pet companion sender ─────────────────────────────────────
//
// Pip-side counterpart to src/pip_link.{h,cpp} on the bigger pets. While
// the listener on Muffin / Visu / Goo-Goo holds the radio always-on,
// Pip is send-only — the radio stays off entirely between events and
// powers up just for the duration of a single transmission (~150 ms).
// Total daily radio time on Pip is on the order of seconds, so the
// 200 mAh battery is essentially unaffected.
//
// Shared 16-byte ESP-NOW packet layout from net.h:
//   [0..3]  magic "GOOG"
//   [4]     version (1)
//   [5]     msgType — 16 = PipTreat, 17..20 reserved for Phase 3+
//   [6..9]  sender id (low 4 bytes of MAC)
//   [10]    animal (0=Bear, 1=Cat, 2=Dog) — informational
//   [11]    language (0=DE, 1=EN) — informational
//   [12..15] event-id (millis at send time) — receiver dedups via this
//
// kind: 0=Apple, 1=Carrot, 2=Bone — see PipTreatKind in pip_link.h.
// animal: bonded animal byte; the pet uses it to colour the floater
//         and pick a matching reaction sound.

namespace pip {
namespace link {

// Throw a treat to any listening pet in radio range. Powers up WiFi +
// ESP-NOW, sends a 3-burst with the same event-id, tears the radio down
// again. Blocking call, ~150-200 ms. Safe to call from the main loop.
void sendTreat(uint8_t kind, uint8_t animal);

// Dispatch a wand-gesture peak count. Same lifecycle as sendTreat —
// brief radio-up, 3-burst, radio-down. The home pet uses the count to
// drive a hop / heart-float animation.
void sendWand(uint8_t peakCount, uint8_t animal);

}  // namespace link
}  // namespace pip
