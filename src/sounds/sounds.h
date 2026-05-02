#pragma once
#include <Arduino.h>

enum class Sound : uint8_t {
  None,
  Greet, Happy, Pet, Excited, Love,
  Yawn, Snore, Startle, Sad, Wake,
  Eat, Tickle, RainShake, UpsideDown, GooGoo,
  CollectFood,
  MediaBabble,
  Singing,
  SayHello,    // wake word detected (voice pipeline is listening)
  SayOkay,     // speech finished (voice pipeline is transcribing)
};

// Plays the given sound through M5.Speaker. Stops anything currently
// playing. Calling with Sound::None silences the speaker.
void playSound(Sound s);

// Globally silence/un-silence sound playback. Used for listen mode where
// the speaker is offline and the mic is reading audio in instead.
void setSoundEnabled(bool enabled);
