#pragma once
#include <stdint.h>

// Pure-logic core of the pet's needs system. Deliberately Arduino/M5-free
// so it can be unit-tested on the native PIO platform without hardware
// stubs. `pet_state.h` re-exports the struct + functions for the rest of
// the firmware.

struct Needs {
  uint8_t happiness;
  uint8_t energy;
  uint8_t fullness;
};

// Lowest of the three (with happiness weighted slightly stronger) is what
// drives the default face mapping in main.
uint8_t computeMood(const Needs& n);

// Decay needs based on elapsed time. Should be called every frame.
// Pass `sleeping=true` while the pet is sleeping (energy regenerates,
// other needs decay slower).
void decayNeeds(Needs& n, uint32_t now_ms, uint32_t& last_decay_ms, bool sleeping);
