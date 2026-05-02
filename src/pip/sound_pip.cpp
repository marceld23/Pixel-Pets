#include "sound_pip.h"

#include <M5Unified.h>

namespace pip {

namespace { bool g_enabled = true; }

void setEnabled(bool enabled) { g_enabled = enabled; }

// Helpers — M5.Speaker.tone() is non-blocking on M5Unified; tones are
// mixed by the speaker worker. Back-to-back calls with delay between
// produce sequenced notes.
static void seq(uint16_t f1, uint32_t d1) {
    M5.Speaker.tone(f1, d1);
}
static void seq(uint16_t f1, uint32_t d1,
                uint16_t f2, uint32_t d2) {
    M5.Speaker.tone(f1, d1);
    delay(d1 + 10);
    M5.Speaker.tone(f2, d2);
}
static void seq(uint16_t f1, uint32_t d1,
                uint16_t f2, uint32_t d2,
                uint16_t f3, uint32_t d3) {
    M5.Speaker.tone(f1, d1); delay(d1 + 10);
    M5.Speaker.tone(f2, d2); delay(d2 + 10);
    M5.Speaker.tone(f3, d3);
}
static void seq(uint16_t f1, uint32_t d1,
                uint16_t f2, uint32_t d2,
                uint16_t f3, uint32_t d3,
                uint16_t f4, uint32_t d4) {
    M5.Speaker.tone(f1, d1); delay(d1 + 10);
    M5.Speaker.tone(f2, d2); delay(d2 + 10);
    M5.Speaker.tone(f3, d3); delay(d3 + 10);
    M5.Speaker.tone(f4, d4);
}

void play(Sound s) {
    if (!g_enabled || s == Sound::None) return;
    switch (s) {
        case Sound::Greet:
            // 3 ascending tones — friendly hello on wake.
            seq(700, 90, 900, 90, 1100, 110);
            break;
        case Sound::TreatCycle:
            // Soft tick — quick double click when the user cycles treat kind.
            seq(1300, 30, 1100, 40);
            break;
        case Sound::Throw:
            // Upward swoosh — four fast rising notes for the throw.
            seq( 600, 35,  900, 35, 1300, 35, 1800, 70);
            break;
        case Sound::Sleepy:
            // Descending, longer — going to sleep.
            seq(900, 200, 700, 200, 500, 300);
            break;
        case Sound::Wake:
            // Upward sweep — waking from sleep.
            seq(500, 100, 700, 100, 1000, 140);
            break;
        case Sound::SplashHi:
            seq(880, 120, 1320, 180);
            break;
        default: break;
    }
}

}  // namespace pip
