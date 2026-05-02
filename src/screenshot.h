#pragma once
#include <Arduino.h>
#include <M5Unified.h>

// Programmatic screenshot dump for README/docs images.
//
// Build with `-DSCREENSHOT_MODE=1` (set on a build_flags level in
// platformio.ini if you want it permanently, or pass on the CLI). The
// pet still runs normally. The current canvas can be dumped as base64
// RGB565 over Serial via three triggers:
//   1. Type `shot` + ENTER in the serial monitor → one dump.
//      Type `shot all`     → auto-dump on every screen / language change.
//      Type `shot stop`    → disable auto-dump again.
//      Type `shot animals` → cycle Bear → Cat → Dog with eyes-open guarantee
//                            and dump each one (file names include the animal).
//   2. (Legacy) Hold PWR ≥ 2 s. Works on CoreS3; on Core2 the AXP192
//      PEK button only fires click events, so the serial trigger is the
//      reliable path there.
// A host tool (`tools/extract_screenshots.py`) decodes the dumps into PNG.
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

// `shot animals` cycles through every animal at runtime — main.cpp must
// register accessors so the screenshot module can switch the rendered
// animal without otherwise depending on pet-state internals.
using GetAnimalFn = uint8_t (*)();
using SetAnimalFn = void    (*)(uint8_t);
void registerAnimalAccessors(GetAnimalFn get, SetAnimalFn set);

// `shot lang de|en` switches the UI language at runtime. Same idea: keep
// screenshot module decoupled from the persisted struct.
using SetLangFn = void (*)(uint8_t);
void registerLangSetter(SetLangFn set);

// Set true while the animals cycle is running. face.cpp checks this in
// drawIdle to suppress the periodic blink so eyes are always open in the
// captured frame.
extern bool g_forceEyesOpen;

#else
inline void begin(M5Canvas*)                          {}
inline void tick(uint32_t, const char*)               {}
inline void dump(const char*)                         {}
inline void registerAnimalAccessors(uint8_t(*)(), void(*)(uint8_t)) {}
inline void registerLangSetter(void(*)(uint8_t)) {}
#endif

}  // namespace screenshot
