#pragma once
#include <Arduino.h>
#include <M5Unified.h>

#include "target_caps.h"

// ─── Pip accessory UI ──────────────────────────────────────────────────────
//
// Pip is a pocket-sized companion device for one of the bigger pets, not a
// fourth pet itself. The display shows the next treat the user will throw
// (Apple / Carrot / Bone), a "Shake me!" hint, and the two button hints.
// Shaking sends an ESP-NOW broadcast to a paired pet via pip_link_send;
// here we only render the UI.

namespace pip {

// Runtime UI state — drives which screen drawPip renders.
enum class UiState : uint8_t {
    Idle      = 0,   // showing the selected treat + "shake me" hint
    Throwing  = 1,   // ~800 ms post-shake animation
    Sleeping  = 2,   // long idle → dim screen with Zzz
};

// Treat selection — values 0..2 match PipTreatKind in pip_link.h so we
// pass the byte straight through to the protocol on a throw.
enum class TreatKind : uint8_t {
    Apple  = 0,
    Carrot = 1,
    Bone   = 2,
};

// Menu page — what's currently shown on Pip. BtnA cycles through these:
//   Empty  → "pick one" placeholder; shaking does nothing (safe carry mode)
//   Apple/Carrot/Bone → throw the matching treat on shake
//   Wand   → gesture mode: each shake increments a peak counter, after
//            ~1 s of stillness the count is dispatched as msgType=18 and
//            the home pet hops that many times
enum class MenuPage : uint8_t {
    Empty  = 0,
    Apple  = 1,
    Carrot = 2,
    Bone   = 3,
    Wand   = 4,
};
constexpr uint8_t kMenuPageCount = 5;

// View struct — pure snapshot data, no logic. Filled per frame in
// main_pip.cpp and handed to drawPip().
struct PipView {
    uint32_t  now_ms;
    UiState   state;
    MenuPage  page;
    uint32_t  throwStartMs;     // when state transitioned to Throwing
    uint8_t   wandPeakCount;    // 0..N, displayed on the Wand page while
                                // peaks are accumulating (resets after dispatch)
    float     tiltX, tiltY;     // gravity EMA — small body lean
    uint8_t   batteryPct;       // 0..100
    bool      charging;
    uint8_t   language;         // 0=DE, 1=EN — drives tr(Str::…) on this side
};

// Splash wordmark on boot. Blocking ~2.5 s, drawn from setup().
void drawSplash(M5Canvas& canvas, uint32_t now_ms, uint32_t start_ms);

// Main per-frame renderer. Picks the right screen based on v.state.
void drawPip(M5Canvas& canvas, const PipView& v);

}  // namespace pip
