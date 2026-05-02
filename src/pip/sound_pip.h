#pragma once
#include <stdint.h>

namespace pip {

// Pip accessory sounds — short tone sequences via M5.Speaker.tone().
// The buzzer on StickC PLUS2 can't sensibly play WAVs.
enum class Sound : uint8_t {
    None        = 0,
    Greet       = 1,    // 3-tone upward chirp on wake / boot
    TreatCycle  = 2,    // soft tick when BtnA cycles the next treat
    Throw       = 3,    // upward swoosh on shake → treat thrown
    Sleepy      = 4,    // descending yawn — going to sleep
    Wake        = 5,    // upward sweep — coming back from sleep
    SplashHi    = 6,    // 2-tone chime on boot splash
};

void setEnabled(bool enabled);
void play(Sound s);

}  // namespace pip
