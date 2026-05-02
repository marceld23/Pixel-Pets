#pragma once
#include <Arduino.h>

#include "target_caps.h"

#if !TARGET_HAS_WIFI
// On targets without WiFi (Pip) this module isn't even pulled in;
// the header stays safely includable so the caller site compiles.
// All functions are no-ops then.
#endif

namespace webradio {

// Stream status. Evaluated by the renderer to show a status indicator
// (Connecting…/Playing/Error/None).
enum class State : uint8_t {
    Off        = 0,   // stream not active
    Connecting = 1,   // WiFi connect or stream open in progress
    Playing    = 2,   // stream playing
    Error      = 3,   // 3× reconnect failed
};

// Language-dependent station selection. Caller passes g_lang (0 = DE, 1 = EN).
// DE → WDR Die Maus, EN → Fun Kids UK.
void start(uint8_t lang);

// Cleanly stop stream, release WiFi reference, stop audio.
void stop();

// Call per frame: ticks the audio stream decoder and checks reconnect
// conditions.
void tick(uint32_t now_ms);

// Current state for the renderer.
State state();

// Volume 0..255 — comes from the existing settings-volume value.
void setVolume(uint8_t vol);

}  // namespace webradio
