#pragma once
#include <Arduino.h>
#include <M5Unified.h>

// Programmatic screenshot dump for README/docs images.
//
// Build with `-DSCREENSHOT_MODE=1` (set on a build_flags level in
// platformio.ini if you want it permanently, or pass on the CLI). The
// pet still runs normally; pressing the PWR button for ≥ 2 s during any
// screen base64-dumps the current canvas as RGB565 over Serial. A host
// tool (`tools/extract_screenshots.py`) decodes the dumps into PNG.
//
// Disabled by default — `SCREENSHOT_MODE` undefined turns all functions
// here into compile-time no-ops, no flash overhead.

namespace screenshot {

#if SCREENSHOT_MODE

// Call once after canvas is created, in setup().
void begin(M5Canvas* canvas);

// Call every loop iteration. Watches for the hotkey (PWR ≥ 2 s) and dumps
// the current canvas if triggered. `screen_name` is included in the dump
// header so the host tool can name files sensibly. Pass nullptr for "current".
void tick(uint32_t now_ms, const char* screen_name);

// Force-dump the canvas right now with the given name. Useful for an
// auto-sequencer that walks through all screens in turn.
void dump(const char* screen_name);

#else
inline void begin(M5Canvas*)                          {}
inline void tick(uint32_t, const char*)               {}
inline void dump(const char*)                         {}
#endif

}  // namespace screenshot
