#include "screenshot.h"
#include "i18n.h"

#if SCREENSHOT_MODE

namespace screenshot {

namespace {

constexpr uint32_t kHotkeyHoldMs = 2000;
constexpr uint32_t kCooldownMs   =  800;
constexpr size_t   kBase64Cols   =   72;

M5Canvas*  g_canvas        = nullptr;
uint32_t   g_lastDumpMs    = 0;
bool       g_armed         = false;       // PWR was pressed; waiting for release
uint32_t   g_pressStartMs  = 0;

// Serial-command state.
constexpr size_t kCmdBufMax = 32;
char       g_cmdBuf[kCmdBufMax];
size_t     g_cmdLen        = 0;
bool       g_autoDumpOnChange = false;
char       g_lastScreen[24] = {0};

// Animal-cycle state. Owned in screenshot.cpp; main.cpp registers the
// accessors so we can switch the rendered animal without including the
// PetState struct.
GetAnimalFn g_getAnimal = nullptr;
SetAnimalFn g_setAnimal = nullptr;
SetLangFn   g_setLang   = nullptr;

struct AnimalCycle {
    bool      active     = false;
    uint8_t   step       = 0;        // 0=arm, 1=Bear→Cat, 2=Cat→Dog, 3=Dog→done
    uint8_t   savedAnimal = 0;
    uint32_t  nextStepMs = 0;
};
AnimalCycle g_cycle;

const char* kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Streaming base64 over Serial — chunks of 3 input bytes → 4 output bytes,
// line-wrapped at kBase64Cols characters. Avoids any RAM growth (no
// String allocation), works for arbitrary canvas sizes.
void streamBase64(const uint8_t* data, size_t len) {
    size_t col = 0;
    char   out[5];
    out[4] = 0;
    for (size_t i = 0; i + 2 < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16 |
                     (uint32_t)data[i + 1] << 8 |
                     (uint32_t)data[i + 2];
        out[0] = kBase64Alphabet[(v >> 18) & 0x3F];
        out[1] = kBase64Alphabet[(v >> 12) & 0x3F];
        out[2] = kBase64Alphabet[(v >>  6) & 0x3F];
        out[3] = kBase64Alphabet[ v        & 0x3F];
        Serial.write(out, 4);
        col += 4;
        if (col >= kBase64Cols) {
            Serial.write('\n');
            col = 0;
        }
    }
    // Tail (1 or 2 leftover bytes)
    size_t tail = len % 3;
    if (tail > 0) {
        size_t i = len - tail;
        uint32_t v = (uint32_t)data[i] << 16;
        if (tail >= 2) v |= (uint32_t)data[i + 1] << 8;
        out[0] = kBase64Alphabet[(v >> 18) & 0x3F];
        out[1] = kBase64Alphabet[(v >> 12) & 0x3F];
        out[2] = (tail == 2) ? kBase64Alphabet[(v >> 6) & 0x3F] : '=';
        out[3] = '=';
        Serial.write(out, 4);
    }
    if (col != 0) Serial.write('\n');
}

}  // namespace

bool g_forceEyesOpen = false;

void registerAnimalAccessors(GetAnimalFn get, SetAnimalFn set) {
    g_getAnimal = get;
    g_setAnimal = set;
}

void registerLangSetter(SetLangFn set) {
    g_setLang = set;
}

void begin(M5Canvas* canvas) {
    g_canvas = canvas;
    Serial.println(F("[screenshot] enabled — type 'shot' (one) | 'shot all' (auto) | 'shot animals' (cycle) | 'shot stop' | or hold PWR >=2 s"));
}

namespace {

// Forward decl — definition lives below in the next anonymous namespace.
void dumpRaw(const char* name);

// Strip trailing whitespace and lower-case a small ASCII command in place.
void normalizeCmd(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
        s[--n] = 0;
    }
    // Skip leading spaces.
    char* p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    for (char* q = s; *q; ++q) {
        if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + ('a' - 'A'));
    }
}

void handleCommand(const char* cmd, const char* screen_name) {
    if (cmd[0] == 0) return;
    if (strcmp(cmd, "shot") == 0) {
        dump(screen_name);
    } else if (strcmp(cmd, "shot all") == 0) {
        g_autoDumpOnChange = true;
        g_lastScreen[0]    = 0;   // force a dump on the next tick
        Serial.println(F("[screenshot] auto-dump on screen change: ON"));
    } else if (strcmp(cmd, "shot stop") == 0 || strcmp(cmd, "shot off") == 0) {
        g_autoDumpOnChange = false;
        if (g_cycle.active) {
            // Restore animal + clear cycle if user aborts.
            if (g_setAnimal) g_setAnimal(g_cycle.savedAnimal);
            g_cycle.active = false;
            g_forceEyesOpen = false;
            Serial.println(F("[screenshot] animals cycle aborted"));
        }
        Serial.println(F("[screenshot] auto-dump on screen change: OFF"));
    } else if (strcmp(cmd, "shot animals") == 0) {
        if (!g_getAnimal || !g_setAnimal) {
            Serial.println(F("[screenshot] animals cycle unavailable: accessors not registered"));
        } else if (g_cycle.active) {
            Serial.println(F("[screenshot] animals cycle already running"));
        } else {
            g_cycle.active      = true;
            g_cycle.step        = 0;
            g_cycle.savedAnimal = g_getAnimal();
            g_cycle.nextStepMs  = millis();
            g_forceEyesOpen     = true;
            Serial.println(F("[screenshot] animals cycle: Bear → Cat → Dog"));
        }
    } else if (strcmp(cmd, "shot lang de") == 0) {
        if (g_setLang) { g_setLang(0); Serial.println(F("[screenshot] lang=de")); }
    } else if (strcmp(cmd, "shot lang en") == 0) {
        if (g_setLang) { g_setLang(1); Serial.println(F("[screenshot] lang=en")); }
    } else if (strcmp(cmd, "shot help") == 0 || strcmp(cmd, "shot ?") == 0) {
        Serial.println(F("[screenshot] commands:"));
        Serial.println(F("  shot           — dump current screen once"));
        Serial.println(F("  shot all       — auto-dump whenever screen/lang changes"));
        Serial.println(F("  shot animals   — cycle Bear→Cat→Dog (eyes open) and dump each"));
        Serial.println(F("  shot lang de   — switch UI language to German"));
        Serial.println(F("  shot lang en   — switch UI language to English"));
        Serial.println(F("  shot stop      — disable auto-dump / abort cycle"));
    }
}

void tickAnimalCycle(uint32_t now_ms) {
    if (!g_cycle.active) return;
    if ((int32_t)(now_ms - g_cycle.nextStepMs) < 0) return;

    // Wait long enough between switches so the next render fully replaces
    // the canvas before we dump it. ~350 ms covers a few frames + any
    // animal-specific intro animation jitter.
    constexpr uint32_t kSettleMs = 350;

    auto qualifiedName = [](const char* animal, char* buf, size_t n) {
        snprintf(buf, n, "pet_%s_%s", animal, g_lang ? "en" : "de");
    };

    char nm[32];

    switch (g_cycle.step) {
        case 0:
            g_setAnimal(0);   // Bear
            g_cycle.nextStepMs = now_ms + kSettleMs;
            g_cycle.step = 1;
            break;
        case 1:
            qualifiedName("bear", nm, sizeof(nm));
            dumpRaw(nm);
            g_setAnimal(1);   // Cat
            g_cycle.nextStepMs = millis() + kSettleMs;
            g_cycle.step = 2;
            break;
        case 2:
            qualifiedName("cat", nm, sizeof(nm));
            dumpRaw(nm);
            g_setAnimal(2);   // Dog
            g_cycle.nextStepMs = millis() + kSettleMs;
            g_cycle.step = 3;
            break;
        case 3:
            qualifiedName("dog", nm, sizeof(nm));
            dumpRaw(nm);
            g_setAnimal(g_cycle.savedAnimal);
            g_cycle.active = false;
            g_forceEyesOpen = false;
            Serial.println(F("[screenshot] animals cycle done"));
            break;
    }
}

void pollSerial(const char* screen_name) {
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            if (g_cmdLen > 0) {
                g_cmdBuf[g_cmdLen] = 0;
                normalizeCmd(g_cmdBuf);
                handleCommand(g_cmdBuf, screen_name);
                g_cmdLen = 0;
            }
        } else if (g_cmdLen + 1 < kCmdBufMax) {
            g_cmdBuf[g_cmdLen++] = (char)c;
        } else {
            // Overflow → drop the line.
            g_cmdLen = 0;
        }
    }
}

}  // namespace

namespace {

// Emit one dump using the exact `name` as-is (no language suffixing). The
// public `dump()` adds the lang suffix; the animals cycle uses this directly
// because it builds its own fully-qualified names.
void dumpRaw(const char* name) {
    if (!g_canvas) return;
    if (!name) name = "current";
    int w = g_canvas->width();
    int h = g_canvas->height();
    const uint8_t* px = (const uint8_t*)g_canvas->getBuffer();
    if (!px) {
        Serial.println(F("[screenshot] canvas buffer not available"));
        return;
    }
    size_t bytes = (size_t)w * (size_t)h * 2;   // RGB565 = 2 bytes/px

    // M5Canvas on ESP32 + M5GFX stores sprite pixels in the panel's wire
    // format, which on M5Stack ILI934x devices is big-endian RGB565. Label
    // it accurately so the host decoder picks the right byte order.
    Serial.printf("--SCREENSHOT-BEGIN name=%s w=%d h=%d fmt=rgb565be bytes=%u\n",
                  name, w, h, (unsigned)bytes);
    streamBase64(px, bytes);
    Serial.println(F("--SCREENSHOT-END"));
    g_lastDumpMs = millis();
}

}  // namespace

void dump(const char* screen_name) {
    if (!screen_name) screen_name = "current";
    // Suffix the screen name with the active UI language so DE and EN
    // captures end up in distinct files (`pet_de_01.png`, `pet_en_01.png`).
    char qualified[40];
    snprintf(qualified, sizeof(qualified), "%s_%s",
             screen_name, g_lang ? "en" : "de");
    dumpRaw(qualified);
}

void tick(uint32_t now_ms, const char* screen_name) {
    if (!screen_name) screen_name = "current";

    // Serial-command trigger (works on every target).
    pollSerial(screen_name);

    // Drive the animals cycle (Bear → Cat → Dog) if active.
    tickAnimalCycle(now_ms);

    // Auto-dump on screen-name OR language change while enabled.
    if (g_autoDumpOnChange &&
        now_ms - g_lastDumpMs >= kCooldownMs) {
        char key[24];
        snprintf(key, sizeof(key), "%s_%s",
                 screen_name, g_lang ? "en" : "de");
        if (strncmp(g_lastScreen, key, sizeof(g_lastScreen) - 1) != 0) {
            strncpy(g_lastScreen, key, sizeof(g_lastScreen) - 1);
            g_lastScreen[sizeof(g_lastScreen) - 1] = 0;
            dump(screen_name);
        }
    }

    // Legacy hotkey: PWR pressed for ≥ kHotkeyHoldMs (works on CoreS3, not Core2).
    if (now_ms - g_lastDumpMs < kCooldownMs) return;
    if (M5.BtnPWR.isPressed()) {
        if (!g_armed) {
            g_armed = true;
            g_pressStartMs = now_ms;
        }
        if (now_ms - g_pressStartMs >= kHotkeyHoldMs) {
            dump(screen_name);
            g_armed = false;
        }
    } else {
        g_armed = false;
    }
}

}  // namespace screenshot

#endif   // SCREENSHOT_MODE
