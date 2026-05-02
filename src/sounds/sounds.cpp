#include "sounds.h"
#include <M5Unified.h>

#include "greet.h"
#include "happy.h"
#include "purr.h"
#include "excited.h"
#include "love.h"
#include "yawn.h"
#include "snore.h"
#include "startle.h"
#include "sad.h"
#include "wakeup.h"
#include "eat.h"
#include "tickle.h"
#include "rain_shake.h"
#include "upside_down.h"
#include "goo_goo.h"
#include "collect_food.h"
#include "media_babble.h"
#include "singing.h"
#include "say_hello.h"
#include "say_okay.h"

namespace {
struct Clip {
  const uint8_t* data;
  size_t         len;
};

Clip lookup(Sound s) {
  switch (s) {
    case Sound::Greet:   return {greet_wav,   greet_wav_len};
    case Sound::Happy:   return {happy_wav,   happy_wav_len};
    case Sound::Pet:     return {purr_wav,    purr_wav_len};
    case Sound::Excited: return {excited_wav, excited_wav_len};
    case Sound::Love:    return {love_wav,    love_wav_len};
    case Sound::Yawn:    return {yawn_wav,    yawn_wav_len};
    case Sound::Snore:   return {snore_wav,   snore_wav_len};
    case Sound::Startle: return {startle_wav, startle_wav_len};
    case Sound::Sad:     return {sad_wav,     sad_wav_len};
    case Sound::Wake:    return {wakeup_wav,  wakeup_wav_len};
    case Sound::Eat:     return {eat_wav,     eat_wav_len};
    case Sound::Tickle:  return {tickle_wav,  tickle_wav_len};
    case Sound::RainShake:  return {rain_shake_wav,  rain_shake_wav_len};
    case Sound::UpsideDown: return {upside_down_wav, upside_down_wav_len};
    case Sound::GooGoo:     return {goo_goo_wav,     goo_goo_wav_len};
    case Sound::CollectFood:return {collect_food_wav, collect_food_wav_len};
    case Sound::MediaBabble:return {media_babble_wav, media_babble_wav_len};
    case Sound::Singing:    return {singing_wav,      singing_wav_len};
    case Sound::SayHello:   return {say_hello_wav,    say_hello_wav_len};
    case Sound::SayOkay:    return {say_okay_wav,     say_okay_wav_len};
    case Sound::None:    return {nullptr, 0};
  }
  return {nullptr, 0};
}
}  // namespace

namespace { bool g_soundEnabled = true; }

void setSoundEnabled(bool enabled) { g_soundEnabled = enabled; }

void playSound(Sound s) {
  if (!g_soundEnabled) return;
  if (s == Sound::None) {
    M5.Speaker.stop();
    return;
  }
  Clip c = lookup(s);
  if (c.data == nullptr || c.len == 0) return;
  // repeat=1 (play once); channel=-1 (auto); stop_current=false so back-to-back
  // events on the same frame don't truncate each other — the M5 mixer has 8
  // channels, so short samples can overlap cleanly.
  M5.Speaker.playWav(c.data, c.len, 1, -1, false);
}
