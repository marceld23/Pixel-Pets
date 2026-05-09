# Pixel Pets — concept

Architecture and gameplay of the virtual pets. **Three pet variants** plus an optional **pocket accessory** (Pip):

| Target | Board | Role | Branding | Voice | Camera | Hard buttons |
|---|---|---|---|---|---|---|
| `cores3` | M5Stack CoreS3 + Module-LLM   | Pet      | "Muffin"  | yes | yes | no (touch UI) |
| `visu`   | M5Stack CoreS3 without module | Pet      | "Visu"    | no  | yes | no (touch UI) |
| `core2`  | M5Stack Core2                 | Pet      | "Goo-Goo" | no  | no  | BtnA/B/C |
| `pip`    | M5StickC PLUS2 (ESP32 PICO)   | Accessory | "Pip"    | no  | no  | BtnA/B (companion device) |
| `pip-s3` | M5StickC PLUS2 (ESP32-S3)     | Accessory | "Pip"    | no  | no  | BtnA/B (companion device) |

Pip is **not** a fourth pet — it's a pocket-sized companion device that pairs with one of the three pets via ESP-NOW (see [Pip companion device](#pip-companion-device-pip-mode) below). Out of radio range it falls back to a small bear face so it's never just a brick.

Hardware setup, flash workflow and module-specific gotchas: per-pet guides under `docs/`, indexed by [`hardware.md`](hardware.md) ([goo-goo](setup-goo-goo.md) · [visu](setup-visu.md) · [pip](setup-pip.md) · [muffin](setup-muffin.md)). Sound asset specs: [`sound_assets.md`](sound_assets.md). Module / build / state architecture: [`architecture.md`](architecture.md).

---

## Needs system

Three values 0..100 replace the single `mood`. Tick every 1 s in [`pet_state.cpp`](../src/pet_state.cpp) → `decayNeeds()`:

| Need | Decay (awake) | Recovery (asleep) |
|---|---|---|
| `happiness` | −1 every 5 s | frozen |
| `energy`    | −1 every 10 s | **+1 every 2 s** (~3.3 min for a full battery) |
| `fullness`  | −1 every 12 s | frozen |

`computeMood(needs)` returns the user-visible mood: primarily driven by `happiness`, but below 30 % each of `energy` and `fullness` drag it down linearly — a starving pet is sad no matter how much petting it gets.

Special cases (override the mood mapping in `updateFaceFromState`):

- `forceSleep == true` → `Sleeping` (manually via BtnC / voice / settings)
- Time-of-day `Night` (between sunset +15 min and sunrise −30 min) → `Sleeping`
- 30-minute session limit reached → bedtime sequence + power-off + 30-minute lockout

---

## Input — per target

### Touch (all targets except Pip)

Classifier against the head ellipse + anatomical regions:

| Zone | Effect |
|---|---|
| Forehead | happiness +8, Pet sound, stroke counter |
| Eye L/R | Startled flash, Startle sound |
| Cheek L/R | happiness +5, blush boost, Happy sound |
| Mouth | fullness +20, eating animation |

Plus gesture detection:

- **Draw a circle** → somersault (hop + spin animation)
- **Two-finger pull** → wobble animation
- **Long hold + snuggle** → hand-warming with body lean

### Bottom strip (CoreS3) or hard buttons (Core2)

On both targets the bottom hint strip ([`drawButtonHints`](../src/face.cpp)) shows three glyphs — on Core2 they label the physical BtnA/B/C in the bezel, on CoreS3 they are the touch-active UI buttons:

| Glyph | Action |
|---|---|
| "Hi!" speech bubble | Greet → happiness +3, energy +2 |
| Smiley | Tickle → happiness +5, laughing face |
| "Zz" | Sleep → forceSleep = true |

On Core2 the actions come from `M5.BtnA/B/C.wasClicked()` polling, gated with `#if TARGET_HAS_HARD_BUTTONS`. On CoreS3 they come from the touch rects in `handleTouchPet`, gated with `#if !TARGET_HAS_HARD_BUTTONS`.

### IMU (all targets)

Gravity EMA `(gx, gy, gz)` + linear acceleration component. Classified into:

- **Petting** (stroke peaks): happiness +5, heart float
- **Horizontal shake**: toy throw, boredom counter
- **Vertical shake**: rain shower + body wash
- **Standing upright**: stand react (Happy + Goo-Goo sound)
- **Upside down**: sad reaction
- **Edge standing**: enters sport mode

### Voice (cores3 only)

Wake word "Muffin" → Whisper transcribes → Qwen3-0.6B classifies into 1–3 tags from a fixed list (eat, pet, love, laugh, sleep, wake, greet, sad, startle, sing, dance, ball/mouse/rattle/butterfly/plush, movie/game/internet/social/friends/radio, idle).

Pipeline: [`voice_pipeline.cpp`](../src/voice_pipeline.cpp). System prompt + tag mapping: top of [`main.cpp`](../src/main.cpp). Robustness measures against LLM hallucinations:

- **Whisper-first bypass**: parse keywords directly on Whisper output first; only fall through to the LLM when nothing matches. ~80 % of common commands skip the LLM entirely and are dispatched in ~50 ms.
- Token limit 64 (forces the model out of the reasoning block)
- `<think>…</think>` stripping before keyword matching
- Garbage filter (recognises code markers, oversized output → IDLE)
- Few-shot prompt with explicit IDLE fallback

Animation sequencing:

- Listen settle: 800 ms after the LLM response, before the first tag is dispatched (lets the listening mimic + SayOkay sound play out)
- Adaptive gap between tags = `flashFace` duration + 250 ms (avoids animation overlap on multi-tag responses)

While web radio is playing, the voice pipeline is paused via `voice::pause()` so Whisper doesn't transcribe the speaker output and trigger ghost tags.

### Camera (cores3 + visu)

Front camera + skin-tone detector ([`face_detect.cpp`](../src/face_detect.cpp)). When a face is detected, no modal is open and the pet isn't sleeping, the pet greets once per cooldown window: happy face + Goo-Goo sound + heart float. While sleeping or in voice listening / web radio playback the camera is fully disabled (`face_detect::setEnabled(false)`) — otherwise every passer-by would wake the pet.

### Photo + Gallery (cores3 + visu)

Tap "Photo" in the media menu → live preview of the front camera with a small pet sprite overlaid in the lower-left corner (pet selfie) and a dedicated **round shutter button on the right edge** (the camera lens sits at the top of the device, so a bottom-center button would be blocked by the user's thumb during the shot). Tap the shutter → 200 ms white flash, JPEG produced by software-encoding the live RGB565 frame via `frame2jpg()` (the GC0308 sensor doesn't support a hardware JPEG mode), saved via [`photo_store.cpp`](../src/photo_store.cpp) on the LittleFS "photos" partition. Up to 5 photos: oldest slot is overwritten round-robin (or a previously freed slot, if the gallery delete created one). Sorted "newest first" via a monotonic NVS order counter. Capture rewards +8 happiness, heart float, `Face::Love` — capped to one reward per 60 s so spamming the shutter doesn't farm happiness; the photo still saves either way.

"Gallery" opens a **3 + 2 thumbnail grid** of the stored photos (drawn at scale 0.25 from the QVGA source). Tap a thumb → fullscreen view with the pet overlay in the lower-left corner; back-X returns to the grid, the trash button + confirm modal deletes the currently shown photo. Opening a photo gives +5 happiness (60 s cooldown), so kids can replay the gallery without unbounded reward farming.

---

## Eyes with gaze tracking

- Sclera background (ellipse), dark pupil, highlight upper-left
- Pupil can move up to ±4 px in any direction
- Gaze logic: while a touch is active → pupils aim at the touch point; idle → saccades every 2–5 s to a random point
- Lerp factor 0.15 per frame
- Blink frequency depends on energy (4 s / 3 s / 2 s)

## Tilt wobble

Head sprite offset `(tiltX, tiltY) = clamp(gx*5, gy*3)` with a low-pass on the gravity EMA. No separate sensor read.

---

## World + weather

- IP-based location detection via `ip-api.com` ([`world.cpp`](../src/world.cpp)), cached 7 days in NVS
- Weather via `open-meteo.com` with real sunrise/sunset times, cached 1 hour
- Moon phase computed locally from the synodic month, only visible in the night scene
- Travel between scenes: Bedroom, Meadow, Forest, Beach, City — each scene gets its own weather
- Background renderer: day / evening / night palettes, dynamic clouds / rain / snow / sandstorm

---

## Mini-games and activities

- **Sport mode**: Squat / Jump / Yoga, IMU-based rep detection, 10 reps default
- **Foraging**: mini-game for collecting apple / carrot / bone items into the inventory
- **Toy play**: Ball / Mouse / Rattle / Butterfly / Plush — each item has its own animation, boredom kicks in after 3 plays with the same toy
- **Media consumption**: Movies / Games / Internet / Social / Friends / **Web radio** — all reduce happiness slightly **except Friends, Web radio, Photo and Gallery**
- **Web radio** (only on targets with `TARGET_HAS_WIFI`, i.e. all except Pip): stream via [`webradio.cpp`](../src/webradio.cpp) with the `ESP32-audioI2S` library. DE → WDR Die Maus, EN → Fun Kids UK; selected by `g_pet.persisted.language`. No negative effects, Excited mimic, pulsing note symbols, gentle body sway (~0.5 Hz). WiFi is held open via a `wifiKeepAlive("webradio")` reference and released on stop. Auto-stop after 30 minutes; manual stop via tap on the media button. On Muffin (cores3) additionally: the voice pipeline is muted via `voice::pause()` while radio is playing (otherwise Whisper transcribes the speaker output), and "RADIO" is recognised as a voice tag — the user can say "turn on the radio" instead of clicking through the menu.
- **Photo + Gallery** (only on targets with `TARGET_HAS_CAMERA`, i.e. Muffin and Visu): see the camera section above for the full flow.
- **Friends mode**: ESP-NOW-based item exchange between two pets in range. Two-step flow: first a **rendezvous handshake** (both pets show a big "Verabreden" button — only when both tap within 5 s of each other does the session advance; 60 s timeout otherwise → NoFriend pose). Then the 4-button send screen with Gift / Heart / Food / Game; up to 5 sends per side, 60 s session window. Received items show up as a strip in front of the pet, each is animated individually, then a celebration. Cooldown 30 s before re-entry. Available on all WiFi targets (Muffin / Visu / Goo-Goo) — Pip has no WiFi.
- **Timer**: a simple egg timer, opened by tapping the clock, loud Goo-Goo + vibration when it expires.

---

## Pip companion device (Pip-Mode)

Pip is a pocket-sized **accessory** for one of the three home pets (Muffin / Visu / Goo-Goo). The kid carries Pip on the go and uses it to throw treats at the home pet over ESP-NOW. There is no standalone-pet fallback any more — Pip is purpose-built for the companion role.

### What Pip does (shipped, verified on hardware)

- **First-run language picker** (DE/EN). On the very first boot Pip shows two flag tiles stacked vertically (German tricolor + St George's cross) — no on-screen text per design. BtnA toggles the highlighted flag, BtnB confirms. The choice is persisted as NVS `"lng"` + `"lcs"` so this never re-runs. All on-Pip text reads from `g_lang` via `tr(Str::…)`, so the selection takes effect on every subsequent screen.
- **5-page menu cycled with BtnA**: `Wähle aus` (empty / safe carry mode) → Apple → Carrot → Bone → Tricks → wrap. The selected page persists to NVS as `"pg"` and survives reboots; default after a fresh flash is `Wähle aus` so an unattended Pip in a pocket doesn't accidentally throw treats.
- **Treat thrower** (Apple / Carrot / Bone pages). Display shows the treat icon centred, name in size 3 below. A wrist-flick triggers a 3-packet ESP-NOW broadcast (`pip_link_send::sendTreat`); within ~200 ms the home pet plays `Face::Eating` + Eat sound + Apple float to fullness + Heart float to happiness, and bumps `+5 happiness` / `+8 fullness`. Throw-side animation: 800 ms treat-flying-up + "Wirf!" flash overlay.
- **Empty / `Wähle aus` page.** Shows a dashed-circle empty-jar glyph with a centred `?`. **Shake events are intentionally ignored** on this page — kid carrying Pip in the pocket can't trigger throws by accident. BtnA cycles to the next page.
- **Tricks page** (gesture-counted hops). Wand-glyph icon, "Auf + ab" hint. Each registered shake peak (= up-or-down flick) increments a counter shown live on the display. After ≥900 ms of stillness the count is dispatched as `kMsgPipWand` (msgType 18) to the home pet, which fires the existing **hop-chain animation** for `peakCount` hops (`g_pet.hopChainCount` set in the listener handler; the renderer animates a parabolic hop per slot, `tickHopChain` plays Laughing face + Tickle giggle sound + Heart float + +2 happiness on each hop transition). Cap on Pip side: 9 peaks per gesture. The same `|a| − 1 g` shake detector feeds both Treats and Tricks; the page just routes the event differently.
- **Sleep on demand.** BtnA long-press (≥600 ms) or BtnB short-press → Sleeping Zzz screen. Auto-sleep after 60 s idle.
- **Light-sleep audio fix.** When the display falls asleep (5 min idle) Pip enters `esp_light_sleep_start` cycles every 1.5 s. We now `M5.Speaker.end()` before each sleep entry and `M5.Speaker.begin()` on wake — without that the buzzer's LEDC channel popped audibly on every wake-sleep transition. Plus the motion-detect noise floor was bumped from 0.05 g to 0.10 g so quiet desk vibrations don't false-wake the device and replay `Sound::Wake` in a loop.
- **Shake-throw reliability.** Shake metric is `|a| − 1 g` (raw acceleration magnitude minus gravity), threshold 0.4 g, single-peak trigger, 1.5 s cooldown. Earlier "linear accel via gravity-EMA subtraction" was replaced because the EMA absorbed the shake itself within a few samples; the deviation-from-1-g metric is orientation-independent and doesn't decay during sustained motion. CPU stays at 240 MHz post-splash and the loop runs at 60 fps so sharp peaks don't fall between IMU polls.
- **Pip-Mode toggle on the home pet** (Settings → page 4) opt-in, default off. Always-on listener costs ~20 % runtime on the bigger pet; that's surfaced honestly in the toggle's body text. Pip itself stays radio-off until each shake (sender powers up the radio for ~150 ms per throw or wand dispatch), so its 200 mAh battery is essentially unaffected.

Pip is treated as **feature-complete** for now. The listener and sender are stable, the protocol reservation (msgType 16..31) leaves room for future companion features without breaking compatibility.

### Other companion ideas (sketched, not on the roadmap)

These were considered during the brainstorming pass but are deliberately deferred — Pip-as-treat-thrower covers the core "kid takes pet on the go" story:

- **Egg / hatchling mode** — Pip starts as an egg, hatches after carrier conditions into a baby pet that transfers to the host.
- **Pet mail** — Pip carries a mood snapshot of the home pet to a friend's house, delivers to the pet there.
- **Remote shutter** — Pip becomes the camera trigger for a Muffin / Visu group selfie.

(The earlier "step counter" and "magic-wand gestures" ideas were dropped: step counting interacts poorly with Pip's pocket use case, and wand gestures would need a much higher IMU sample rate than 60 Hz.)

### Implementation map

| Concern | Files | Notes |
|---|---|---|
| Listener (home pet side) | `src/pip_link.{h,cpp}` | begin / end / pause / resume / tick + recv ring + dedup + msgType dispatch. `friendsBegin` / `webradio::start` pause it via the existing hooks. |
| Sender (Pip side) | `src/pip/pip_link_send.{h,cpp}` | Single-shot `sendTreat(kind, animal)`: power up WiFi → init ESP-NOW → 3-packet burst with same eid → tear down. ~150 ms total per call. |
| Pip UI | `src/pip/face_pip.{h,cpp}` | `UiState` (Idle / Throwing / Sleeping) drives per-screen render. `TreatKind` (Apple / Carrot / Bone) values match the wire protocol. |
| Pip orchestration | `src/main_pip.cpp` | Shake detector, button handler, NVS persist of `selectedTreat`, mobile power strategy. |
| Pet-side toggle UI | `src/face.cpp` (page 3 render) + `src/main.cpp` touch handler | Page 4 of Settings ("Pip-Modus"). `Persisted::pipMode` (NVS key `"pip"`) survives reboots. |

### Protocol (shared with Friends mode — see `net.h`)

16-byte packet, magic `GOOG`, dedup via 4-byte event-id. Pip-link owns msgType range 16..31; Friends uses 0..7. The asymmetric-RF reliability machinery carries over for free: standard 802.11 B/G/N at max TX power, `WIFI_AP_STA` mode (so the ESP32-S3 receiver doesn't drop broadcasts), the dedup ring, and unicast TX after the partner's MAC is known. (1.0.0 used `WIFI_PROTOCOL_LR` instead, but LR was not reliably interoperable across ESP32 ↔ ESP32-S3 — switched to B/G/N in 1.0.1.) Currently only `kMsgPipTreat = 16` is wired; the other reservations (17 step report, 18 wand, 19 egg check-in, 20 shutter) are documented in `pip_link.h` and unused.

### Commit lineage

| Commit | Phase | Content |
|---|---|---|
| `deb5e81` | 1 | Skeleton + persisted toggle + lifecycle hooks (no-op listener) |
| `448081c` | 2A | Refactor `openEspNowRadio` / `closeEspNowRadio` out of `friendsBegin/End` |
| `cbe3afa` | 2B | Listener body + msgType 16 + main.cpp handler |
| `66cb069` | 2C | Pip-side sender + shake-gesture trigger |
| `f0ac653` | 2D | Pip accessory UI redesign + shake reliability tuning |

---

## Session limit (parental control)

- Default 30 min per boot (configurable in the parent web page, 5–120 min, `g_pet.persisted.sessionLimitMin`)
- After expiry: 3 s bedtime announcement + 6 s sleep animation, then `M5.Power.powerOff()`
- 30-minute lockout in NVS (`playLockoutEndSec`) — fixed value, blocks the next boot
- The manual PWR button uses the same bedtime sequence but **without** lockout commit (parent override)

---

## Time of day (RTC + sun)

`M5.Rtc.getDateTime()` + sunrise/sunset from open-meteo:

| Phase | Window |
|---|---|
| Morning | 30 min before sunrise → 60 min after |
| Day | 60 min after sunrise → 60 min before sunset |
| Evening | 60 min before sunset → 15 min after |
| Night | otherwise |

If the RTC isn't set and we're offline: defaults to `Day`. If the RTC is set but world data is missing: heuristic by hour.

The `Night` phase ⇒ the pet falls asleep automatically.

---

## Persistence (NVS / Preferences)

Namespace `"pet"` — see [`pet_state.cpp`](../src/pet_state.cpp) `loadPersisted()` / `savePersisted()`. Saved every 60 s and on modal transitions.

Important keys:

- `hap`, `eng`, `ful` — needs
- `bornY/M/D`, `seenY/M/D` — birth date + last-seen date
- `lck` — session lockout end (Unix epoch)
- `lim` — session limit per boot
- `lng` — language
- `ani` — animal choice (bear/cat/dog)
- `tim*` — timer state
- `inv*` — foraging inventory

The world cache lives in a separate `"world"` namespace ([`world.cpp`](../src/world.cpp)).

The photo storage uses a separate `"photos"` namespace + the LittleFS `photos` partition (cores3/visu only).

---

## Authors

- **Justus** and **Marcel** — see also the credits screen in the menu.
