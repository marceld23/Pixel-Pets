# Pixel Pets — sound assets

Spec and inventory of all embedded WAV samples.

## Technical requirements

| Parameter | Value |
|---|---|
| Format | uncompressed WAV (PCM) |
| Channels | mono |
| Sample rate | 22050 Hz or 16000 Hz (consistent per sample) |
| Bit depth | 16-bit signed |
| Loudness | normalised to ~ −3 dBFS peak |

## Delivery / embedding

All samples are embedded as C headers under [`../src/sounds/<name>.h`](../src/sounds/) via `xxd -i` directly into flash rodata (LittleFS was considered and dropped — embedding is more robust, doesn't fail when the FS partition gets corrupted). The `.h` files define `<name>_wav` (`uint8_t[]`) and `<name>_wav_len` (`size_t`).

Total footprint: ~5.5 MB of flash for all samples. The partition tables ([`../partitions_cores3_16MB.csv`](../partitions_cores3_16MB.csv) for cores3/visu, [`../partitions_core2_16MB.csv`](../partitions_core2_16MB.csv) for core2) configure 7.5–7.94 MB OTA app slots so the samples fit alongside the firmware.

Played via `M5.Speaker.playWav(data, len, repeat=1, channel=-1, stop_current=false)` in [`../src/sounds/sounds.cpp`](../src/sounds/sounds.cpp). `stop_current=false` lets short samples overlap — the M5 mixer has 8 channels.

Pip uses tone sequences via `M5.Speaker.tone()` instead — see [`../src/pip/sound_pip.cpp`](../src/pip/sound_pip.cpp).

## Sample list

### Universal (cores3 / visu / core2)

| File | Trigger |
|---|---|
| `greet.wav` | Greet action (BtnA / touch / voice / boot) |
| `happy.wav` | Happy reaction (petting, cheek touch, stand-upright) |
| `purr.wav` | (defined, currently unused — could be re-enabled for the stroke loop) |
| `excited.wav` | Excited mood, toy play, friends-item gift |
| `love.wav` | Love action, long-time-no-see at boot, `MEDIA_FRIENDS` |
| `yawn.wav` | Sleep action, transition to sleepy |
| `snore.wav` | Sleeping loop every 4 s |
| `startle.wav` | Startle action, eye touch, "boo" voice |
| `sad.wav` | Sad action, mood crash, stand-upside-down |
| `wakeup.wav` | Wake action, waking up from sleep |
| `eat.wav` | Eat action, mouth touch, foraging item consumed |
| `tickle.wav` | Laugh action, tickle touch, toy play |
| `rain_shake.wav` | Vertical shake → rain shower |
| `upside_down.wav` | Pet held upside down |
| `goo_goo.wav` | Boot babble, idle babble, timer expiry, camera greeting |
| `collect_food.wav` | Foraging: item collected |
| `media_babble.wav` | Media consumption (Movie/Game/Internet/Social), Dance action |
| `singing.wav` | Singing mode (upright + L/R tilt) |

### Voice-specific (cores3 only, `TARGET_HAS_LLM`)

| File | Trigger |
|---|---|
| `say_hello.wav` | Wake word "Muffin" recognised — played in `onVoiceWake()` as a "I hear you" cue |
| `say_okay.wav` | VAD speech-end — played in `onVoiceSpeechEnd()` as a "got it, thinking" cue |

These two samples are compiled into core2 / visu builds too (a few KB of flash), but never referenced — the voice pipeline call sites are gated behind `#if TARGET_HAS_LLM`.

## Style guide

- No speech — only tonal "pet" sounds (Tamagotchi-, Furby-, small-cartoon-animal-style).
- Higher and cute rather than low and growly (except `purr`, `snore`, `yawn`).
- No hard transients (no clicks/pops at the start).
- 5 ms fade-in / fade-out against pops.

## Adding new samples

1. Normalise the WAV file to the style guide.
2. `xxd -i name.wav > src/sounds/name.h`
3. In the generated header, change `unsigned char` to `const unsigned char` (flash rodata).
4. Extend the `Sound` enum in [`../src/sounds/sounds.h`](../src/sounds/sounds.h).
5. Extend the `lookup()` switch in [`../src/sounds/sounds.cpp`](../src/sounds/sounds.cpp).
6. Trigger from `main.cpp` / `face.cpp` via `playSound(Sound::NewName)`.

---

## Authors

- **Justus** and **Marcel** — see also the credits screen in the menu.
