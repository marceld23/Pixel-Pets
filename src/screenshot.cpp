#include "screenshot.h"

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

void begin(M5Canvas* canvas) {
    g_canvas = canvas;
    Serial.println(F("[screenshot] enabled — hold PWR >=2 s to dump current screen"));
}

void dump(const char* screen_name) {
    if (!g_canvas) return;
    if (!screen_name) screen_name = "current";
    int w = g_canvas->width();
    int h = g_canvas->height();
    const uint8_t* px = (const uint8_t*)g_canvas->getBuffer();
    if (!px) {
        Serial.println(F("[screenshot] canvas buffer not available"));
        return;
    }
    size_t bytes = (size_t)w * (size_t)h * 2;   // RGB565 = 2 bytes/px

    // Header → host tool parses these fields with simple string ops.
    Serial.printf("--SCREENSHOT-BEGIN name=%s w=%d h=%d fmt=rgb565le bytes=%u\n",
                  screen_name, w, h, (unsigned)bytes);
    streamBase64(px, bytes);
    Serial.println(F("--SCREENSHOT-END"));
    g_lastDumpMs = millis();
}

void tick(uint32_t now_ms, const char* screen_name) {
    // Hotkey: PWR pressed for ≥ kHotkeyHoldMs.
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
