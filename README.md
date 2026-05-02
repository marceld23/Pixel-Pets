<p align="center">
  <img src="assets/logo.jpg" alt="Pixel Pets logo" width="220">
</p>

# Pixel Pets

<p align="center">
  <a href="docs/concept.md">Concept</a> ·
  <a href="docs/architecture.md">Architecture</a> ·
  <a href="docs/hardware.md">Hardware</a> ·
  <a href="docs/sound_assets.md">Sound assets</a> ·
  <a href="CREDITS.md">Credits</a> ·
  <a href="LICENSE">License</a>
</p>

A family of virtual pets on M5Stack hardware. **Three pet variants** — **Muffin** (CoreS3 + LLM), **Visu** (CoreS3 alone), **Goo-Goo** (Core2) — plus one optional **accessory**, **Pip** (M5StickC PLUS2), which acts as a pocket-sized companion device for any of the bigger pets. One source tree, five build envs (`cores3` / `visu` / `core2` / `pip` / `pip-s3`). Pet logic, animations, mini-games, ESP-NOW friends and weather/location are target-agnostic; voice and the front camera are CoreS3-only.

**Version 1.0.0** — first stable release. See [Version history](#version-history) at the bottom for the rough scope.

## Pick your pet animal

Each device renders the same set of animals — **Bear, Cat, Dog** — chosen on first boot (and changeable later in Settings). Real screenshots from a Goo-Goo (Core2):

| Bear | Cat | Dog |
|:---:|:---:|:---:|
| ![Bear](screenshots/pet_bear.png) | ![Cat](screenshots/pet_cat.png) | ![Dog](screenshots/pet_dog.png) |

🐻 **Live demo + landing page**: <https://marceld23.github.io/Pixel-Pets/>. The site has a click-and-poke browser pet, an overview of all variants and the get-started guide. Source under [`site/`](site/), deployed via GitHub Actions on every push.

## Highlights

- **Three animals × three pet variants** — pick **Bear / Cat / Dog** on first boot (changeable later in Settings), running on **Muffin** (CoreS3 + LLM), **Visu** (CoreS3 alone) or **Goo-Goo** (Core2). Same source tree.
- **World-aware pet** — at boot the device looks up your **location** (IP geolocation via ip-api), pulls **real weather**, **sunrise / sunset times** and the **moon phase** from open-meteo, and adapts the scene to the actual time of day at your location: pale-blue **Morning** sky → clear **Day** sky → muted **Evening** dusk → dark **Night** with a crescent moon. The sun and clouds tint with the same phase. All cached in NVS so it survives reboots offline.
- **Battery-backed clock + NTP sync** — wall-clock time is synced once a day over Wi-Fi and persisted by the RTC in between, so the time-of-day rendering and the parental session limit work even with no network on a given day.
- **Voice control** *(Muffin only)* — wake word **"Muffin"**, offline **Whisper** speech-to-text + **Qwen3-0.6B** intent classifier running on the Module-LLM expansion. Plain sentences ("eat something", "let's dance", "turn on the radio") trigger matching actions. No cloud, no audio leaves the device.
- **Front camera + selfies** *(Muffin / Visu)* — proximity-wake when you walk past, photo button overlays the pet on a selfie, 5-slot LittleFS gallery with delete.
- **ESP-NOW Friends + Pip accessory** — two pets in range pair with a synchronised tap and exchange gifts / hearts / food / toys over long-range ESP-NOW (no router). The optional **Pip** (M5StickC PLUS2) acts as a pocket-sized treat thrower: pick Apple / Carrot / Bone with BtnA, wrist-flick to throw, home pet eats it within ~200 ms.
- **Web radio** — WDR Die Maus (DE) / Fun Kids UK (EN) in the media menu, pet sways to the music; voice-triggerable on Muffin.
- **Mini-games + scenes** — squat / jump / yoga workouts, butterflies / mushrooms / surf / scorpion / asteroids / cross-the-street per scene, foraging for apple / berry / fish, five toys with boredom mechanics, scene travel between Bedroom / Meadow / Forest / Beach / City / Desert / Space.
- **Parent dashboard** — captive-portal Wi-Fi setup, optional **`<pet>-setup.local`** web server for live stats and remote-edit of the daily play-session limit (5–120 min). Session expiry triggers a 30-minute lockout that survives reboots.
- **Eleven moods + gestures** — Idle / Happy / Excited / Love / Sleepy / Sleeping / Sad / Startled / Laughing / Eating / Speaking, driven by happiness / energy / fullness needs. Touch zones (forehead / cheeks / mouth / ears), IMU-based petting / shake / stand reactions, somersault on circle drag, sing-and-applause when tilted upright.

See the [Version history](#version-history) for the full per-target breakdown.

## Targets

The three pets:

| Target | Board | Branding | Voice | Camera | Hard buttons | Display | Audio | Web radio |
|---|---|---|---|---|---|---|---|---|
| `cores3` | M5Stack CoreS3 + Module-LLM    | "Muffin"  | ✅ Wake word + Whisper + Qwen3 | ✅ | ❌ Touch strip | 320×240 | WAV | ✅ |
| `visu`   | M5Stack CoreS3 without module  | "Visu"    | ❌ | ✅ | ❌ Touch strip | 320×240 | WAV | ✅ |
| `core2`  | M5Stack Core2                  | "Goo-Goo" | ❌ | ❌ | ✅ BtnA/B/C | 320×240 | WAV | ✅ |

The accessory:

| Target | Board | Role | Display | Audio |
|---|---|---|---|---|
| `pip`    | M5StickC PLUS2 (ESP32 PICO)   | Pocket-sized companion to a bigger pet — sends treats and gestures over ESP-NOW. Doubles as a tiny pet face when out of range. | 135×240 | Buzzer |
| `pip-s3` | M5StickC PLUS2 (S3 revision)  | Same role, ESP32-S3 board variant. | 135×240 | Buzzer |

## Hardware shopping list

Pick **one pet** plus optionally **a Pip** as a pocket-sized accessory. Direct links to the M5Stack official store; all USB-C, no soldering, no breadboard.

### Pets — pick one

| Pet | Required parts | M5Stack store links |
|---|---|---|
| **Muffin** | M5Stack CoreS3 + **M5Stack Module LLM (AX630C)** + **Battery Module 13.2** (1500 mAh — separate purchase, not bundled with the LLM module). | [CoreS3](https://shop.m5stack.com/products/m5stack-cores3-esp32s3-iot-development-kit) · [Module LLM (AX630C)](https://shop.m5stack.com/products/m5stack-llm-large-language-model-module-kit-ax630c) · [Battery Module 13.2](https://shop.m5stack.com/products/battery-module-13-2-1500mah) |
| **Visu** | M5Stack CoreS3 + **M5GO Battery Bottom3** (500 mAh, official CoreS3 accessory). | [CoreS3](https://shop.m5stack.com/products/m5stack-cores3-esp32s3-iot-development-kit) · [Battery Bottom3](https://shop.m5stack.com/products/m5go-battery-bottom3-for-cores3-only) |
| **Goo-Goo** | M5Stack Core2 (built-in 500 mAh battery, no extra parts). | [Core2](https://shop.m5stack.com/products/m5stack-core2-esp32-iot-development-kit-v1-1) |

### Pip — optional pocket accessory

| | |
|---|---|
| **Pip** | M5StickC PLUS2 (built-in 200 mAh battery, no extra parts). [StickC PLUS2](https://shop.m5stack.com/products/m5stickc-plus2-esp32-mini-iot-development-kit) |

Pip is **not** a fourth pet. It's a tiny ESP-NOW remote that pairs with any of the three home pets above. Display shows the next treat the kid will throw (Apple / Carrot / Bone, BtnA cycles); a quick wrist-flick fires an ESP-NOW broadcast and within ~200 ms the home pet eats the treat (`Face::Eating` + Eat sound + Apple/Heart floats + happiness/fullness boost).

Pet-mode owners enable reception via **Settings → Pip mode** (page 4). The toggle is off by default — the always-on listener costs ~20 % battery runtime on the bigger pet, so opt-in is honest. Pip itself stays radio-off until each shake (sender powers up the radio for ~150 ms per throw), so its 200 mAh battery is essentially unaffected.

The treat-thrower flow is shipped end-to-end and verified on hardware. See [`src/pip_link.h`](src/pip_link.h) (listener API), [`src/pip/pip_link_send.h`](src/pip/pip_link_send.h) (Pip-side sender). Other companion ideas (egg/hatchling, pet-mail, remote shutter) are sketched in [`docs/concept.md`](docs/concept.md) but not on the immediate roadmap — Pip is treated as feature-complete for now.

Web radio (in the media menu) plays **WDR Die Maus** in German and **Fun Kids UK** in English — no negative side effects, with note-symbol animation and body sway. On cores3 it can also be triggered by voice command ("turn on the radio").

On Muffin and Visu additionally: **Photo** and **Gallery** in the media menu. Photo opens the front camera with the pet overlaid in the lower-left corner (pet selfie), tap = capture. Gallery walks through the last five photos with left/right-tap navigation. Both reward happiness without negative effects.

## Build & flash

```bash
pio run -e cores3                              # Muffin (CoreS3 + LLM, default)
pio run -e cores3 -t upload                    # Muffin build + flash
pio run -e visu                                # Visu (CoreS3 alone)
pio run -e core2                               # Goo-Goo (Core2)
pio run -e pip                                 # Pip accessory (StickC PLUS2 PICO)
pio run -e pip-s3                              # Pip accessory (StickC PLUS2 S3)
pio run -e cores3 -e visu -e core2 -e pip-s3   # build all
```

Detailed hardware/flash notes (COM-port conflicts, Module-LLM setup, gotchas):
👉 [`docs/hardware.md`](docs/hardware.md)

## Repository layout

```
src/
  target_caps.h            ← TARGET_HAS_LLM / HAS_CAMERA / HAS_HARD_BUTTONS / ...
  main.cpp                 ← orchestrator (setup + loop)
  face.{h,cpp}             ← renderer (PetView-driven)
  pet_state.{h,cpp}        ← needs, RTC, persistence, touch zones
  voice_pipeline.{h,cpp}   ← wake/VAD/Whisper/Qwen3   (HAS_LLM only)
  face_detect.{h,cpp}      ← front-camera face detection + JPEG capture (HAS_CAMERA only)
  photo_store.{h,cpp}      ← LittleFS-backed selfie storage (HAS_CAMERA only)
  webradio.{h,cpp}         ← MP3 stream decoder (HAS_WIFI only)
  net.{h,cpp}              ← WiFi, captive portal, NTP, ESP-NOW friends, parent server
  world.{h,cpp}            ← IP geolocation + open-meteo + moon phase
  i18n.{h,cpp}             ← string table (single language at runtime)
  sounds/                  ← embedded WAV headers (xxd -i)
  pip/                     ← StickC Plus 2-only renderer + tone-based sound engine
  main_pip.cpp             ← Pip orchestrator (replaces main.cpp on the pip env)

docs/
  concept.md               ← gameplay & high-level design
  architecture.md          ← software architecture (modules, build flow, state)
  hardware.md              ← hardware setup + flashing + Module-LLM workflow
  sound_assets.md          ← WAV sample spec and trigger map

platformio.ini             ← [env:cores3] + [env:core2] + [env:visu] + [env:pip]
partitions_cores3_16MB.csv ← partition table for CoreS3 / Visu (incl. LittleFS)
partitions_core2_16MB.csv  ← partition table for Core2
partitions_pip_8MB.csv     ← partition table for StickC Plus 2
```

## Documentation

- [`docs/concept.md`](docs/concept.md) — how the pet ticks: needs, input (touch / IMU / hard buttons / voice / camera), world, mini-games, parental session limit, persistence.
- [`docs/architecture.md`](docs/architecture.md) — software architecture: module overview, build-target system, state model, render pipeline.
- [`docs/hardware.md`](docs/hardware.md) — per-target hardware setup, build commands, COM-port notes, Module-LLM ADB workflow, Qwen3 quirks.
- [`docs/sound_assets.md`](docs/sound_assets.md) — WAV spec, sample list, trigger map (Pip uses tone sequences instead).

## Continuous integration

GitHub Actions in [`.github/workflows/ci.yml`](.github/workflows/ci.yml):

- **Matrix build** for all four firmware targets (`cores3` / `core2` / `visu` / `pip`). Fails any PR that breaks a build.
- **Native unit tests** (`pio test -e native`) for the pure-logic modules in [`src/needs_logic.cpp`](src/needs_logic.cpp). Tests live under [`test/test_needs_logic/`](test/test_needs_logic/) and use Unity. Run locally with `pio test -e native` (Linux/macOS, or Windows with MinGW gcc on PATH).

## Screenshots for the docs

The firmware has a built-in canvas dumper, gated behind the dedicated **`*-shots` envs** in [`platformio.ini`](platformio.ini) so it never lands in production builds. Flash one of `cores3-shots` / `visu-shots` / `core2-shots`, navigate to the screen you want to capture and **hold the PWR button ≥ 2 s** — the canvas is dumped over Serial as base64 RGB565. The host-side decoder in [`tools/extract_screenshots.py`](tools/extract_screenshots.py) turns it into PNGs.

Python tooling is managed with [`uv`](https://docs.astral.sh/uv/) — see [`tools/README.md`](tools/README.md) for the full workflow.

## Authors

A father-and-son project: **Justus Dütscher** (10) — ideas, design decisions, tireless field-testing — and **Marcel Dütscher** (Papa) — translating Justus's ideas for the AI doing the actual coding, plus the bits of technical know-how that don't fit on a kid's whiteboard. The first beta was finished on a Sunday afternoon; the boring polish was Papa's evening work.

Every line of firmware was written by AI assistants — primarily **Claude (Anthropic)** — under our direction. `git log --format="%an %ae"` shows which model touched which commit.

See [`CREDITS.md`](CREDITS.md) for the full story, and the in-app credits screen via Settings → Credits.

## Repository naming

Originally this repo was called `muffin` (the CoreS3 flagship pet). The umbrella project name is now **Pixel Pets**, and the repo can be renamed to `pixel-pets` on GitHub at any time — GitHub redirects the old URL automatically. Local clones don't need to be re-cloned; `git remote set-url origin <new>` is enough.

## Version history

### 1.0.0 — May 2026

First stable release. Three pet variants and one accessory, all verified end-to-end on hardware.

**The pets** (Muffin / Visu / Goo-Goo):

- 11 moods driven by happiness / energy / fullness needs
- Touch zones (forehead / cheeks / eyes / mouth) + IMU-based petting / shake / stand reactions
- Gestures: somersault on circle drag, wobble on two-finger pull, snuggle on long hold
- Mini-games per scene (squat / jump / yoga / butterflies / mushrooms / surf / scorpion / asteroids / cross-the-street), foraging for apple/berry/fish, toy play with five toys + boredom, scene travel between Bedroom / Meadow / Forest / Beach / City / Desert / Space
- Singing + applause: tilt sequence triggers the pet to sing, mic listens for a loud peak afterwards
- Real weather + sunrise/sunset (open-meteo) and local moon-phase, IP-based location lookup (ip-api), all cached in NVS
- Egg-timer screen with vibration + Goo-Goo cue when it expires
- Parental session limit (5–120 min, configurable via parent web server) with a 30-minute lockout after expiry
- ESP-NOW Friends mode between any two home pets — rendezvous handshake, gift exchange, LR PHY for asymmetric-RF reliability

**Muffin only** (CoreS3 + Module-LLM):

- Wake word "Muffin" + on-device speech recognition (Whisper) + Qwen3-0.6B intent classifier
- Whisper-first bypass for ~80 % of commands (sub-second latency, no LLM round-trip)
- Voice-triggerable actions (eat, sleep, dance, radio …)
- TTS / wake-WAV side effects neutralised on the LLM module

**Muffin / Visu** (CoreS3 with front camera):

- Front camera + face / proximity detection — pet greets you when you walk past
- Photo + 5-slot gallery (LittleFS), pet-overlay selfies

**Goo-Goo / Visu / Muffin** (everything but Pip):

- Web radio (WDR Die Maus / Fun Kids UK), pet sways to the music
- Captive-portal WiFi setup
- Optional parent web server (mDNS `<pet>-setup.local`) for stats + session-limit edits

**The accessory** (Pip — M5StickC PLUS2):

- Pocket companion that pairs with any of the three home pets via ESP-NOW
- Five-page menu (BtnA cycles): "Wähle aus" (safe-carry mode, no shake response) → Apple → Carrot → Bone → Tricks
- Treat thrower: wrist-flick fires a 3-packet broadcast, home pet eats it within ~200 ms
- Tricks: peak-counted gesture → home pet visibly hops N times in a row (giggle sound + heart per landing)
- First-run language picker (DE / EN flags only, no text)
- Long-press BtnA = sleep; auto-sleep after 60 s idle; sleep-click regression on the buzzer fixed

**Other lanes that grew along the way:**

- GitHub Pages site rebuilt around "three pets + one accessory" framing, click-and-poke browser pet, hardware shopping list
- CI matrix builds all 5 envs on every push; native unit tests for the pure-logic needs module

## License

[MIT License](LICENSE) — open source, free to use, modify and redistribute.

Note: the web radio feature links against [`schreibfaul1/ESP32-audioI2S`](https://github.com/schreibfaul1/ESP32-audioI2S), which is GPL v3.0. Binaries built from the `cores3` / `core2` / `visu` envs therefore inherit GPL v3.0 obligations when redistributed. The `pip` env doesn't link the audio library and stays MIT all the way through. Source code in this repository is MIT regardless of which env you build. See [`LICENSE`](LICENSE) for the full third-party-licenses block.
