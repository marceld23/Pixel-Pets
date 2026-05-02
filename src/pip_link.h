#pragma once
#include <Arduino.h>

#include "target_caps.h"

// ─── Pip-Link — passive ESP-NOW companion listener ─────────────────────────
//
// Phase 1 skeleton. The bigger pets (Muffin / Visu / Goo-Goo) host a
// background ESP-NOW listener that receives packets from a paired Pip
// (StickC PLUS2). Pip is the sender — short bursts during specific
// interactions (treat thrown by shake, step report, wand gesture, …).
// The pet routes incoming packets into local pet-state actions
// (treat received → animation + happiness boost, etc.).
//
// Packets share the existing ESP-NOW protocol used by Friends mode
// (see net.cpp): 16-byte payload, magic "GOOG", msgType + senderId +
// animal + lang + 4-byte event-id. Pip-Link reserves msgType range
// 16..31 — Friends uses 0..7. Same dedup ring works for both.
//
// Lifecycle:
//   - `begin(true)` once at boot if `persisted.pipMode` is set.
//   - `pause()` / `resume()` when another feature claims the radio
//     exclusively (Friends session, web radio on a different channel).
//   - `tick(now)` every main-loop iteration.
//   - `end()` on a clean shutdown.
//
// Phase 1 contract: **all of the above are no-ops** beyond toggling an
// internal flag — no radio is touched. The skeleton exists so call
// sites can be wired in without behaviour changes. Phase 2 fills in
// the actual ESP-NOW listener.
//
// Targets: Pip-Link only makes sense on a pet that listens for a Pip,
// so it's gated on `TARGET_HAS_WIFI` (excludes Pip itself, where the
// counterpart is `pip_link_send`). Header is includable everywhere,
// the namespace functions compile to no-ops on non-WIFI targets.

namespace pip_link {

// Start (or stop) the listener. `enabled` mirrors the persisted toggle;
// passing false from the same toggle later is the clean shutdown path.
// Phase 1: only updates the internal flag; real radio init lands in Phase 2.
void begin(bool enabled);
void end();

// Reflects the current run state. False after end() / before begin(true)
// or while paused.
bool isActive();

// Pause/resume — used by friendsBegin/End and webradio::start/stop so
// the listener doesn't fight other features for the radio. Phase 1: no-ops.
// pause() is idempotent; multiple pause() calls require equal resume()s? No
// — keep it simple: pause/resume are level-triggered, not ref-counted.
// Friends and webradio never overlap with each other (single-modal UI).
void pause();
void resume();

// Per-loop tick. Drains the receive queue and dispatches via the
// registered handlers below.
void tick(uint32_t now_ms);

// ─── Protocol — Pip-link msgTypes ───────────────────────────────────────
//
// Reuses the 16-byte ESP-NOW packet layout shared with Friends mode (see
// net.h). Friends owns msgType 0..7; Pip-link reserves 16..31.
constexpr uint8_t kMsgPipTreat       = 16;   // payload[12]: kind 0/1/2
constexpr uint8_t kMsgPipStepReport  = 17;   // payload[12..13]: uint16 steps  — Phase 3
constexpr uint8_t kMsgPipWand        = 18;   // payload[12]: gesture id        — Phase 3
constexpr uint8_t kMsgPipEggCheckin  = 19;   // payload[12..15]: ms since hatch — Phase 3
constexpr uint8_t kMsgPipShutter     = 20;   // — Phase 3

// Treat kinds (payload byte for kMsgPipTreat).
enum PipTreatKind : uint8_t {
    PipTreatApple   = 0,
    PipTreatCarrot  = 1,
    PipTreatBone    = 2,
};

// Handler signature for an incoming treat. senderAnimal is informational
// (the Pip transmits the bonded animal byte so the pet can pick a matching
// floater colour or sound).
using TreatHandler = void(*)(PipTreatKind kind, uint8_t senderAnimal,
                             uint32_t now_ms);

// Register the treat handler. Pass nullptr to unregister. Phase 1: handler
// is never called. Phase 2 wires this from main.cpp to the +5 happiness /
// Heart float / Excited face / Eat sound combo.
void setTreatHandler(TreatHandler fn);

// Handler signature for an incoming wand gesture. peakCount is how many
// up/down peaks the user produced on the Pip side (1..255). The home pet
// typically makes the pet hop / float that many hearts, capped to a
// reasonable max in the handler.
using WandHandler = void(*)(uint8_t peakCount, uint8_t senderAnimal,
                            uint32_t now_ms);

// Register the wand handler. Pass nullptr to unregister.
void setWandHandler(WandHandler fn);

}  // namespace pip_link
