# Pixel Pets — software architecture

How the codebase is organised, how it builds for four very different M5Stack targets from a single source tree, and how the runtime state flows. Companion to [`concept.md`](concept.md) (gameplay) and [`hardware.md`](hardware.md) (hardware setup).

## High-level layout

The project is a single-repo, single-branch ESP32 firmware that produces five distinct binaries — three pets and one accessory (with two MCU revisions):

| Env | Hardware | Role | Purpose | Sets define |
|---|---|---|---|---|
| `cores3` | M5Stack CoreS3 + Module-LLM (M140) | Pet      | "Muffin" — full feature set with voice + camera | `TARGET_CORES3` |
| `visu`   | M5Stack CoreS3 (no LLM module)     | Pet      | "Visu" — same hardware, voice removed | `TARGET_VISU` |
| `core2`  | M5Stack Core2                      | Pet      | "Goo-Goo" — hard-button target | `TARGET_CORE2` |
| `pip`    | M5StickC PLUS2 (ESP32 PICO)        | Accessory | "Pip" — pocket companion to a bigger pet (sender-side ESP-NOW) | `TARGET_PIP` |
| `pip-s3` | M5StickC PLUS2 (ESP32-S3)          | Accessory | Same role as `pip`, S3 board variant | `TARGET_PIP` |

```
                    ┌──────────────────────┐
                    │  src/target_caps.h   │  one #define per capability
                    └──────────┬───────────┘
                               │
        ┌──────────────────────┼──────────────────────┐
        ▼                      ▼                      ▼
  cores3 / visu /           cores3 only            pip only
  core2 share most       voice_pipeline.cpp      pip/face_pip.cpp
  of src/ (face.cpp,     face_detect.cpp         pip/sound_pip.cpp
  pet_state.cpp, etc.)   webradio.cpp            main_pip.cpp
        │                photo_store.cpp                │
        ▼                                               ▼
   ┌──────────┐                                    ┌──────────┐
   │ main.cpp │                                    │main_pip..│
   └──────────┘                                    └──────────┘
```

## Capability-flag system

[`src/target_caps.h`](../src/target_caps.h) is the single source of truth for what each build can do. Every other module asks it via `#if TARGET_HAS_…`:

```
TARGET_HAS_LLM           1 = voice pipeline (KWS / VAD / Whisper / Qwen3)
TARGET_HAS_CAMERA        1 = front camera + face detection + photo storage
TARGET_HAS_HARD_BUTTONS  1 = physical BtnA/B/C
TARGET_HAS_TOUCH         1 = touchscreen
TARGET_HAS_WAV_AUDIO     1 = WAV-capable speaker (otherwise PWM buzzer only)
TARGET_HAS_WIFI          1 = WiFi features used (NTP, ip-api, web radio, ESP-NOW friends, captive portal)
TARGET_DISPLAY_W / _H    display dimensions
TARGET_NAME              "Muffin" / "Visu" / "Goo-Goo" / "Pip"
TARGET_AP_NAME           captive-portal SSID
TARGET_MDNS_NAME         mDNS hostname
```

Each env in [`platformio.ini`](../platformio.ini) sets exactly one `TARGET_*=1` build flag (e.g. `-DTARGET_CORES3=1`); the header derives every capability from there. Adding a new target = one new branch in `target_caps.h` plus one new `[env:…]` section.

PlatformIO's `build_src_filter` excludes hardware-specific source files from builds where they don't apply:

```ini
[env:core2]
build_src_filter =
    +<*>
    -<voice_pipeline.cpp>     ; no LLM module
    -<face_detect.cpp>        ; no camera
    -<pip/>                   ; pip-only renderer
    -<main_pip.cpp>

[env:pip]
build_src_filter =
    +<*>
    -<main.cpp>               ; pip has its own main
    -<face.cpp>               ; 320×240 renderer doesn't fit 135×240
    -<face_detect.cpp>
    -<voice_pipeline.cpp>
    -<webradio.cpp>           ; no WiFi by design
    -<sounds/>                ; uses tone sequences instead
```

Where `build_src_filter` excludes a file, the call sites in `main.cpp` are **also** wrapped in `#if TARGET_HAS_…` so the linker doesn't see references to symbols that aren't compiled in.

## Source tree

```
src/
  target_caps.h              capability macros (described above)
  main.cpp                   orchestrator for cores3 / core2 / visu
  main_pip.cpp               orchestrator for pip
  face.h / face.cpp          full 320×240 renderer (pet, scenes, modals)
  pet_state.h / .cpp         needs, RTC, NVS, mood computation
  i18n.h / .cpp              string table (single language at runtime)
  net.h / .cpp               WiFi, captive portal, NTP, ESP-NOW friends, parent server
  world.h / .cpp             IP geolocation + open-meteo + moon
  voice_pipeline.h / .cpp    KWS / VAD / Whisper / Qwen3 — cores3 only
  face_detect.h / .cpp       front-camera face detection + JPEG capture — HAS_CAMERA
  photo_store.h / .cpp       LittleFS-backed selfie storage (round-robin, max 5) — HAS_CAMERA
  webradio.h / .cpp          MP3 stream decoder via ESP32-audioI2S — HAS_WIFI
  pip_link.h / .cpp          ESP-NOW companion-listener (pairs the bigger pets
                             with a Pip) — HAS_WIFI, opt-in via Persisted::pipMode
  wifi_config.h              optional pre-baked WiFi credentials (gitignored)
  sounds/                    embedded WAV headers (xxd -i) + sounds.{h,cpp} dispatcher
  pip/                       pip-specific subsystem
    face_pip.h / .cpp        135×240 bear renderer with 5 mimics
    sound_pip.h / .cpp       tone-sequence sound engine
    pip_tuning.h             pip-specific IMU thresholds and timings

partitions_cores3_16MB.csv   16 MB layout for cores3 / visu (incl. LittleFS for photos)
partitions_core2_16MB.csv    16 MB layout for core2
partitions_pip_8MB.csv       8 MB layout for pip
platformio.ini               4 envs sharing common config blocks

docs/                        this folder — concept / hardware / architecture / sounds
.github/workflows/ci.yml     matrix build for all four envs
```

## Module breakdown

### Orchestrators — `main.cpp` / `main_pip.cpp`

The single biggest file in the repo. `main.cpp` (~5 800 lines) drives cores3 / core2 / visu; `main_pip.cpp` (~600 lines) is a much smaller dedicated loop for pip.

`main.cpp` is structured roughly as:

1. Includes + capability-gated imports
2. `SYSTEM_PROMPT` for the LLM (cores3 only)
3. Tunables (motion thresholds, cooldowns, brightness levels)
4. The big `PetState` struct — every piece of session state the pet view tracks
5. Helper functions for state transitions (face, sounds, floats, decay, persistence sync)
6. Per-mode touch handlers (`handleTouchPet`, `handleTouchSettings`, `handleTouchMediaSelect`, …)
7. Per-mode renderers (or thin wrappers around face.cpp)
8. Voice / camera / radio modules wired in via `#if TARGET_HAS_…`
9. `setup()` and `loop()` at the very bottom

`loop()` is the single dispatch point: read input, classify motion, run subsystem ticks (voice, photo, radio, friends, sport, foraging, parental limit), update the face state, render, sleep until next frame.

### Renderer — `face.cpp` (320×240)

A pure off-screen-canvas renderer. Every frame:

1. `render(now)` is called from `loop()`.
2. It selects which screen to render based on the current modal flags (`mediaSelectMode`, `cameraMode`, `galleryMode`, `friendsMode`, …) and falls back to the pet view.
3. The pet view is built from a `PetView` snapshot struct (the renderer never reads `g_pet` directly — `main.cpp` fills the struct each frame, this keeps the renderer purely functional).
4. Background scene → pet body → modal overlays (button hints, gift bar, status bubbles, floats) → top toolbar (clock, battery, settings/timer/media buttons).
5. The canvas is `pushSprite(0, 0)` to the actual M5 display.

The renderer is hardcoded for 320×240 and not used on pip. Pip has its own much smaller renderer in [`src/pip/face_pip.cpp`](../src/pip/face_pip.cpp) (~500 lines) that draws a single bear with five mimics on a 135×240 portrait canvas.

### Pet state + persistence — `pet_state.{h,cpp}`

Two-layer state:

- **Session state** in `PetState` (declared in `main.cpp`) — flags, timers, mode toggles, floats; lost on reboot.
- **Persisted state** in `Persisted` (declared in `pet_state.h`) — `Needs` (happiness/energy/fullness), birth date, last-seen date, language, animal choice, brightness, volume, session limit, foraging inventory, timer state, lockout end. Saved every 60 s and on modal transitions to NVS namespace `"pet"`.

`computeMood(needs)` is a pure function: returns 0..100. `decayNeeds(needs, now, last_decay, sleeping)` is called every frame; it ticks once per second internally.

NVS keys are short (3 chars max) to fit comfortably below the 15-char limit. Photo metadata uses its own `"photos"` namespace (HAS_CAMERA). World cache uses `"world"`.

### Voice pipeline — `voice_pipeline.{h,cpp}` (cores3 only)

Wraps `M5Module-LLM` over Serial2 / Port C. Lifecycle:

1. `voice::begin(Serial, cfg)` → blocks during the full setup (KWS, VAD, Whisper, LLM). Run from a FreeRTOS task on Core 0 so the splash animation on Core 1 can keep ticking.
2. `voice::update()` — drains the message queue every loop iteration. Dispatches callbacks: `onWake`, `onSpeechEnd`, `onTranscribed`, `onTags`.
3. **Whisper-first bypass**: when Whisper transcribes, the parser runs the keyword map directly on the transcription. If a non-IDLE tag is matched, it's dispatched immediately and the LLM is **not** called. ~80 % of common commands save the LLM round-trip.
4. **LLM fallback** for the remaining ~20 % (semantic cases like "I'm tired" → SLEEP). Output goes through:
   - `<think>…</think>` stripping (Qwen3 emits an empty thinking block even with `/no_think`)
   - garbage filter (rejects code-like / oversized / multi-line responses → IDLE)
   - keyword matching against the same map as in step 3
5. `voice::pause()` / `resume()` mute the callbacks without tearing down the module setup. Used by the web radio so Whisper doesn't transcribe the speaker output.

Tags are converted to actions in `applyVoiceTag()` in `main.cpp`. They go through a small queue with adaptive gaps (each animation runs to completion before the next starts).

### Face detection + photo capture — `face_detect.{h,cpp}` (cores3 + visu)

Initialises the CoreS3 GC0308 front camera in RGB565 / QVGA mode for the always-on skin-tone face-detection tick. Two new APIs were added for the photo feature:

- `acquireFrame() / releaseFrame()` — live-preview path; gives the caller an RGB565 frame buffer to push to the display.
- `captureJpeg(out, len)` — switches the sensor to JPEG / quality=10 for one shot, throws away three warm-up frames so the encoder settles, copies the JPEG to a heap buffer, switches back.

The face-detection tick is suspended (`setEnabled(false)`) during camera mode, gallery mode, sleep, voice listening and web radio playback — anywhere a passer-by detection would be wrong.

### Photo storage — `photo_store.{h,cpp}` (cores3 + visu)

5-slot rotating storage on the LittleFS `photos` partition (~960 KB). Maintains a monotonic order counter in NVS namespace `"photos"`, so the gallery can iterate "newest first" regardless of physical slot layout. `save()` prefers empty slots, otherwise overwrites the oldest. `deleteByDisplayIndex()` frees a slot in place; the next save reuses it. `readByDisplayIndex()` pulls the JPEG into a heap-allocated read buffer (grows on demand) so callers don't have to manage lifecycle.

### Pip-link companion listener — `pip_link.{h,cpp}` (HAS_WIFI)

Background ESP-NOW listener that lets a bigger pet (Muffin / Visu / Goo-Goo) receive packets from a paired Pip. Currently wired: `kMsgPipTreat` (16) — shake-thrown treat, decoded into a `TreatHandler` callback that animates Eating face + Apple/Heart floats + happiness/fullness boost on the pet. msgType range 16..31 is reserved for Pip-link; Friends uses 0..7. Receiver-side dedup via a 5-slot eid ring shared in pattern with Friends.

Reuses the Friends-mode 16-byte packet format (magic `GOOG`, msgType + senderId + animal + lang + 4-byte event-id) and the same radio setup: standard 802.11 B/G/N PHY in `WIFI_AP_STA` mode at full TX power, channel 6, all power-save off (1.0.0 used `WIFI_PROTOCOL_LR` here, but LR turned out not to be reliably interoperable across ESP32 ↔ ESP32-S3 — see the 1.0.1 changelog in `README.md`). The `openEspNowRadio` / `closeEspNowRadio` helpers in `net.cpp` are shared between Friends and Pip-link so the radio setup stays identical.

Opt-in via `Persisted::pipMode` (Settings → page 4 toggle). When the toggle flips, `pip_link::begin / end` is called from the touch handler — the listener registers the ESP-NOW recv-cb and adds the broadcast peer; on disable, both are torn down and the radio is closed.

The listener cooperates with the other WiFi consumers via `pause / resume`:

- `friendsBegin` calls `pip_link::pause()` because the friends-mode hard reset re-initialises the ESP-NOW stack and would otherwise pull the listener's recv-cb out from under it. `friendsEnd` resumes.
- `webradio::start` calls `pip_link::pause()` because the AP's WiFi channel rarely matches the ESP-NOW broadcast channel (6) — packets from Pip wouldn't arrive while STA holds the radio on the router's channel. `webradio::stop` resumes.

Pip-side counterpart lives in [`src/pip/pip_link_send.{h,cpp}`](../src/pip/pip_link_send.h) — a single-shot `sendTreat(kind, animal)` that powers up WiFi, fires a 3-packet burst with the same eid, and powers down again (~150 ms total per call). Pip is send-only; it never holds the radio open continuously, so its 200 mAh battery is essentially unaffected.

Treat-thrower flow is shipped end-to-end (commits `deb5e81`/`448081c`/`cbe3afa`/`66cb069`/`f0ac653`) and verified on hardware. The other reserved msgTypes (17 step report, 18 wand, 19 egg check-in, 20 shutter) are sketched in `pip_link.h` for future use but are not on the active roadmap.

### Web radio — `webradio.{h,cpp}` (HAS_WIFI)

Wraps `schreibfaul1/ESP32-audioI2S` (pinned to v3.0.12 because newer releases use C++20 features that the arduino-esp32 gcc-8.4 toolchain can't handle). Two-step state machine: `Off → Connecting → Playing → (Error after 3 failed reconnects)`.

`start(lang)` releases `M5.Speaker` (which uses I2S), claims the I2S peripheral for the audio decoder, sets pins from the saved `M5.Speaker.config()`, and connects to the language-specific stream URL (DE → WDR Maus, EN → Fun Kids UK). On stop the order is reversed — `M5.Speaker.begin()` reclaims I2S.

WiFi is held open via `wifiKeepAlive("webradio")` (see net.cpp); released on stop.

### Networking — `net.{h,cpp}`

Captive-portal WiFi setup, NTP sync, ESP-NOW friends mode, the optional parent web server, and a reference-counted `wifiKeepAlive(reason)` / `wifiRelease(reason)` API used by web radio (and prepared for friends / parents). When the last reference drops, WiFi powers off automatically.

The captive portal serves an HTML config page at `<TARGET_NAME>-setup` SSID (see the branding in `target_caps.h`); after the user submits credentials, the device reboots into STA mode.

### World data — `world.{h,cpp}`

IP-based location resolution via `ip-api.com` (cached 7 days in NVS), weather + sunrise/sunset via `open-meteo.com` (cached 1 hour), local moon-phase computation from the synodic month. None of this is essential for the pet to function — when offline, the rendering falls back to heuristic time-of-day.

### Internationalisation — `i18n.{h,cpp}`

A flat string table indexed by an enum. Every entry stores `{de, en}`. The active language is `g_lang` (0 = DE, 1 = EN), set from `g_pet.persisted.language`. `tr(Str::SomeKey)` returns a `const char*` for the active language.

For the device branding, instead of two hard-coded strings, C99 string-literal concatenation is used: `TARGET_NAME " hört Radio"` resolves at compile time to the target-specific string. Saves runtime branching, costs zero RAM.

### Sound dispatch — `sounds/sounds.{h,cpp}`

Trivial wrapper around `M5.Speaker.playWav()`. The `Sound` enum maps to `<name>_wav` byte arrays that are embedded into flash via `xxd -i`. See [`sound_assets.md`](sound_assets.md) for the full list and triggers.

Pip uses [`src/pip/sound_pip.{h,cpp}`](../src/pip/) instead — small `M5.Speaker.tone()` sequences, no WAV samples.

## Build flow

```
PlatformIO env (cores3 / visu / core2 / pip)
    │
    ├── -DTARGET_<TARGET>=1   (build flag)
    │
    ▼
src/target_caps.h
    │
    ├── derives TARGET_HAS_LLM, HAS_CAMERA, HAS_TOUCH, HAS_WIFI, …
    │   plus TARGET_NAME / TARGET_AP_NAME / TARGET_MDNS_NAME
    │
    ▼
build_src_filter excludes inappropriate .cpp files
    │
    ▼
Source files use `#if TARGET_HAS_…` to gate call sites and conditionally
include feature headers
    │
    ▼
Same single set of source files produces 4 firmware binaries
```

CI builds all four envs in a matrix; any push or PR that breaks one env fails the run. See [`.github/workflows/ci.yml`](../.github/workflows/ci.yml).

## Runtime flow (cores3, simplified)

```
boot
 │
 ├─ M5.begin                   display, IMU, RTC, audio
 ├─ loadPersisted              NVS → Persisted
 ├─ loadWorldCache              NVS → cached city/weather/moon
 ├─ connectAndSyncTime          WiFi → NTP → ip-api/open-meteo → wifiPowerOff
 ├─ voice::begin (async task)   Serial2 → KWS / VAD / Whisper / LLM setup
 ├─ face_detect::begin          camera RGB565 / QVGA
 ├─ photo_store::begin          mount LittleFS (format on first boot)
 ├─ runStartscreen              splash, waits for voice setup with timeout
 ├─ runFirstRunPickers          first-time only: language / animal pickers
 │
 ▼
loop()                          ~30 fps
 │
 ├─ M5.update                   button polling, touch
 ├─ updateMotion(now)           IMU classifier → events 0=none, 1=stroke, 2=shake, 3=vshake
 ├─ voice::update               drains Module-LLM messages → callbacks
 ├─ dispatchPendingTag          one queued voice tag at a time (adaptive gap)
 ├─ subsystem ticks             webradio::tick, friendsTick, parentServerTick,
 │                              face_detect::tick, sport detection, foraging update,
 │                              activity update, weather, dirt, hop chain, sing
 ├─ updateCameraState           camera enabled iff awake && not sleeping && not voice-busy
 ├─ decayNeeds                  pet_state.cpp 1 Hz tick
 ├─ media decay loop            negative-effect media (Movies/Games/Internet/Social)
 ├─ updateFaceFromState          mood → face mapping with overrides
 ├─ savePersisted (every 60 s)
 │
 ├─ render(now)                 ── decides which screen ──
 │                                   ├─ pet view (face.cpp drawFace)
 │                                   ├─ media-select / friends / camera / gallery / settings / …
 │                                   └─ pushSprite to display
 │
 └─ delay until next frame
```

On pip, `main_pip.cpp` runs a similar but much smaller loop with fewer subsystem ticks — no voice, no camera, no friends, no foraging, no sport. Single bear, five mimics, IMU-driven actions, tone sounds, NVS persistence.

## Dependencies

| Library | Version | Used on | Why |
|---|---|---|---|
| `m5stack/M5Unified` | latest | all | hardware abstraction for display, IMU, RTC, audio, power, touch |
| `bblanchon/ArduinoJson` | latest | all | LLM message parsing, weather API, parent-server form |
| `m5stack/M5Module-LLM` | latest | cores3 | Serial2 protocol to the M140 module |
| `schreibfaul1/ESP32-audioI2S` | 3.0.12 | cores3 / core2 / visu | MP3 stream decoder for web radio |
| (Arduino-ESP32 builtins) | bundled | all | WiFi, HTTPClient, ESPmDNS, WebServer, Preferences, LittleFS, esp_camera, esp_now |

## Testability

Most of the firmware is hardware-coupled (display, IMU, camera, speaker, voice module) and therefore tested manually on real devices. A few modules are however pure C++ logic and worth unit-testing:

- [`src/needs_logic.cpp`](../src/needs_logic.cpp) — `computeMood()`, `decayNeeds()`. Extracted out of `pet_state.cpp` so it has zero Arduino / M5 dependencies. Tests live in [`test/test_needs_logic/`](../test/test_needs_logic/).

The `[env:native]` environment in `platformio.ini` builds and runs Unity tests on the host compiler. Locally:

```bash
pio test -e native
```

In CI: a separate matrix entry runs `pio test -e native --verbose` on every push.

The `parseTags` parser in `voice_pipeline.cpp` would also be a good candidate but currently uses `String` from Arduino — pulling it into a similar pure-logic module is on the future-work list.

## Documentation screenshots

The firmware has a built-in canvas dumper, gated behind the dedicated `cores3-shots` / `visu-shots` / `core2-shots` envs in `platformio.ini`. When `SCREENSHOT_MODE` is defined, holding PWR ≥ 2 s base64-dumps the current `M5Canvas` over Serial; [`tools/extract_screenshots.py`](../tools/extract_screenshots.py) decodes the dumps into PNGs.

Production builds (`cores3`, `core2`, `visu`, `pip`) never define `SCREENSHOT_MODE`, so the helper module ([`src/screenshot.cpp`](../src/screenshot.cpp)) compiles to no-ops and adds zero flash overhead.

## Key design choices

- **Single source tree, multiple targets** instead of forks. Adding a 5th target (e.g. AtomS3) is mostly a `target_caps.h` branch + a `[env:…]` block.
- **Capability flags, not target flags**, in feature gates. `#if TARGET_HAS_LLM` reads better and survives renames; `#if TARGET_CORES3` would couple individual features to specific boards.
- **Pure-function renderer** (`face.cpp` reads `PetView`, never `g_pet`) makes the rendering testable in isolation in principle, and keeps the main loop's mutation in one place.
- **NVS for tiny things, LittleFS for blobs** (photos). NVS has a 4 KB blob limit per key — JPEGs go to the dedicated `photos` partition.
- **Reference-counted WiFi keep-alive** — multiple features (web radio, friends, parent server) want WiFi for different reasons; managing this with shared booleans got fragile, the ref counter is robust.
- **Whisper-first bypass** for voice — short-circuits the unreliable Qwen3-0.6B for the 80 % of inputs that map directly to a German keyword. The LLM stays as semantic fallback.
- **Embedded sound assets**, not LittleFS — file-system corruption is more common than firmware corruption, and audio playback shouldn't fail because the FS partition is unhappy.

---

## Authors

- **Justus** and **Marcel** — see also the credits screen in the menu.
