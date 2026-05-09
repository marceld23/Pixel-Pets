# AGENTS.md — instructions for AI coding agents

Drop-in context file for AI coding agents (Claude Code, Cursor, Copilot, Codex, …) working on this repository. Distilled from the project docs and many hours of human-AI co-development. Read this once before your first edit, refer back when a task touches an unfamiliar area.

If something here contradicts the user's explicit instruction in the conversation, follow the user.

---

## What this project is

**Pixel Pets** — a family of virtual pets running on M5Stack ESP32 hardware. One source tree, four firmware binaries:

| Env | Hardware | Pet name | Highlights |
|---|---|---|---|
| `cores3` | M5Stack CoreS3 + Module-LLM (AX630C) | Muffin   | Wake-word voice control + offline LLM, front camera, touchscreen, web radio |
| `visu`   | M5Stack CoreS3 (no LLM module)        | Visu     | Same hardware as Muffin, voice removed, camera + touch + web radio still in |
| `core2`  | M5Stack Core2                         | Goo-Goo  | BtnA/B/C + touch, web radio, no voice, no camera |
| `pip` / `pip-s3` | M5StickC PLUS2 (PICO or S3)   | Pip      | Pocket-sized, 135×240, BtnA/B, buzzer audio, no WiFi |

The umbrella project name is **Pixel Pets**. The repo was originally named `muffin` (the flagship pet) and may still be called that locally.

For the gameplay model see [`docs/concept.md`](docs/concept.md), for the software architecture [`docs/architecture.md`](docs/architecture.md). Hardware setup is per-pet — start at [`docs/hardware.md`](docs/hardware.md) which indexes [`setup-goo-goo.md`](docs/setup-goo-goo.md), [`setup-visu.md`](docs/setup-visu.md), [`setup-pip.md`](docs/setup-pip.md) and [`setup-muffin.md`](docs/setup-muffin.md).

---

## Quick commands

### Build

```bash
pio run -e cores3                          # default — Muffin (CoreS3 + LLM)
pio run -e visu                            # Visu — CoreS3 without LLM
pio run -e core2                           # Goo-Goo — Core2
pio run -e pip                             # Pip — StickC PLUS2 (ESP32 PICO)
pio run -e pip-s3                          # Pip — StickC PLUS2 (S3 revision)
pio run -e cores3 -e visu -e core2 -e pip-s3   # build all four
```

After a non-trivial change, **build all four** (or at least `core2`, `cores3`, `visu`, `pip-s3`). Single-target builds will not catch capability-flag regressions in code only one target compiles.

For maximum speed, fire each `pio run -e <env>` as an independent background task. They don't conflict.

### Flash

```bash
pio run -e <env> -t upload --upload-port COM<N>
```

`pio device list` shows attached COM ports. CoreS3 normally enumerates as `303A:1001`, Core2 as a CH9102 bridge, StickC PLUS2 (PICO) as a CH9102 too, StickC PLUS2 S3 alternates between `303A:8120` (normal) and `303A:1001` (download mode — hold BtnA + reset). See [`docs/setup-pip.md`](docs/setup-pip.md) for the full StickC-S3 download-mode dance.

Two devices on different ports can be flashed in parallel — `pio run -e core2 -t upload --upload-port COM3` and `... --upload-port COM7` simultaneously work fine.

### Monitor

```bash
pio device monitor --port COM<N> --baud 115200
```

Two devices in parallel: open two terminals (or run two `pio device monitor` with different `--port`). Friends-mode debugging needs both pets visible at once.

### Test

```bash
pio test -e native
```

Runs Unity tests against the pure-logic modules ([`src/needs_logic.cpp`](src/needs_logic.cpp)) on the host compiler. CI runs this on every push.

### CI

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) builds the matrix (cores3 / core2 / visu / pip) and runs `pio test -e native`. Any push that breaks one env fails the run. Don't merge through a red CI.

---

## Capability-flag system — required reading

[`src/target_caps.h`](src/target_caps.h) is the single source of truth for what each build can do. Every other module gates code via `#if TARGET_HAS_…`:

```c
TARGET_HAS_LLM            // voice pipeline (cores3 only)
TARGET_HAS_CAMERA         // front camera + face detection + photos (cores3 + visu)
TARGET_HAS_HARD_BUTTONS   // physical BtnA/B/C (core2, pip)
TARGET_HAS_TOUCH          // touchscreen (cores3, visu, core2)
TARGET_HAS_WAV_AUDIO      // WAV speaker (everywhere except pip)
TARGET_HAS_WIFI           // WiFi features (NTP, weather, web radio, ESP-NOW friends)
TARGET_DISPLAY_W / _H     // display dimensions
TARGET_NAME / TARGET_AP_NAME / TARGET_MDNS_NAME   // branding
```

Every env in [`platformio.ini`](platformio.ini) sets exactly one `TARGET_<board>=1` flag (e.g. `-DTARGET_CORES3=1`); the header derives every capability from there. **Adding a new target is one new branch in `target_caps.h` plus one new `[env:…]` section.**

`build_src_filter` in `platformio.ini` excludes hardware-specific `.cpp` files where they don't apply — and call sites in `main.cpp` are also wrapped in `#if TARGET_HAS_…` so the linker doesn't see references to symbols that aren't compiled in. **Both** are required; one without the other is a build error.

When you add a new feature, ask: which capability flag gates it? If the answer is "all targets", no flag needed. If a flag exists, gate on the flag, not on `TARGET_<board>`.

---

## Source-code conventions

### Comments are English-only

Past mixed German + English comments were translated to English in commit `f84b940`. **Don't reintroduce German.**

Two specific exceptions inside `src/voice_pipeline.cpp` — German user-input examples (`"ich bin müde"`) inside English comments are deliberate. They document the kind of natural-language sentence the LLM fallback handles. Leave them.

User-facing strings in [`src/i18n.cpp`](src/i18n.cpp) are bilingual on purpose (`{de, en}` tuples). Those are not comments — leave their German alone.

### Comments-quality bar

Default to writing **no comments**. Only add one when the *why* is non-obvious: a hidden constraint, a subtle invariant, a workaround for a specific bug, behaviour that would surprise a reader. If removing the comment wouldn't confuse a future reader, don't write it.

Don't explain *what* the code does — well-named identifiers already do that. Don't reference the current task or PR ("added for X", "fixes #123"); that belongs in the commit message and rots fast.

### Renderer is pure

[`src/face.cpp`](src/face.cpp) (the 320×240 renderer) **never reads `g_pet` directly**. Each frame, [`src/main.cpp`](src/main.cpp) builds a `PetView` snapshot struct and hands it to the renderer. Keeps mutation in one place and would let the renderer be tested in isolation. Don't break this. Same for the pip renderer in [`src/pip/face_pip.cpp`](src/pip/face_pip.cpp).

### i18n discipline

[`src/i18n.h`](src/i18n.h) declares an enum of string keys; [`src/i18n.cpp`](src/i18n.cpp) has a parallel `kStrings[]` table with a `static_assert` on the array size. Adding a new string = add the enum entry **and** the table entry; the `static_assert` will catch a mismatch at compile time.

Active language is `g_lang`, set from `g_pet.persisted.language` (0 = DE, 1 = EN). Use `tr(Str::Key)` to look up.

For device branding (e.g. `"Goo-Goo hört Radio"`), use C99 string-literal concatenation: `TARGET_NAME " hört Radio"` resolves at compile time.

### NVS keys are short

Three characters max (e.g. `hap`, `eng`, `ful`, `lck`, `ani`). Stay below the 15-char NVS key limit and keep the wear leveling reasonable.

### No "future-proof" abstractions

A bug fix doesn't need a refactor. A one-shot operation doesn't need a helper. Three similar lines is better than a premature abstraction. Don't add error handling for cases that can't happen — trust internal code; only validate at boundaries (user input, external APIs).

### No backwards-compat hacks

We don't ship versioned firmware to a user base. If you remove a feature, delete the code. No `// removed` comments, no renamed `_unused` vars, no flag-gated dead paths.

---

## Friends mode — RF reliability (don't accidentally regress)

The Friends gift-exchange feature uses ESP-NOW on channel 6. The 1.0.1
release rewrote the radio stack after a long debugging session that
exposed several layered issues — directional RF asymmetry between two
Core2s during development, then near-total failure between Core2 ↔
CoreS3 once Muffin was added to the test set. The fix bundle below is
what finally produced reliable 5/5 + 5/5 across every pet pair, in
order of importance:

1. **`WIFI_PROTOCOL_11B|11G|11N`** in `openEspNowRadio` (NOT LR). 1.0.0
   used `WIFI_PROTOCOL_LR` for the +12 dB sensitivity, but Espressif's
   proprietary LR PHY is **not reliably interoperable across ESP32 ↔
   ESP32-S3** — packets sent in LR didn't decode on the partner side,
   which broke every Core2 ↔ CoreS3 / Visu pair. Standard B/G/N at
   max TX power has plenty of margin for two pets in the same room.
   **Don't switch back to LR** without re-running the cross-target
   matrix.
2. **`WIFI_AP_STA`** instead of `WIFI_STA` in `openEspNowRadio`. The
   ESP32-S3 stack silently drops most ESP-NOW broadcast frames in
   disconnected-STA mode. AP_STA gives it an AP MAC table to dispatch
   broadcasts against. Without this, Muffin + Visu show `rxCb` stuck
   at 0 while sending fine.
3. **Unicast TX once partner MAC is known.** In the Sending state we
   capture the source MAC from the first received packet and
   `esp_now_add_peer()` it as a unicast peer; from then on
   `friendsSendPacket()` addresses the partner directly instead of
   broadcasting. 802.11 unicast has MAC-layer ACK + retry; broadcast
   is fire-and-forget and the CoreS3 RX path frequently drops it.
4. **Reach-back Ready** in the Sending state. The faster tapper used
   to go silent the moment they matched, leaving the slower partner
   stuck on "Waiting for friend" until the 60 s timeout. Now Sending
   keeps emitting `kMsgRendezvousReady` until something
   content-bearing (item or `kMsgSessionDone`) arrives from the
   partner.
5. **Single-packet bursts + jittered re-broadcasts.** 1.0.0 used
   3-packet item bursts and 4-packet match bursts at fixed 20-ms
   spacing. Two pets tapping simultaneously would burst into each
   other and lose all their packets to RF collision. 1.0.1 sends 1
   packet per event and lets the periodic re-broadcaster (~500 ms
   item, ~700 ms done, ~800 ms reach-back, all jittered ±300 ms) do
   the redundancy work. Jitter breaks lockstep with the partner
   within a couple of beats.
6. **Receiver-side dedup** via a 5-slot eid ring — same eid arriving
   multiple times = same gift, count once. Unchanged from 1.0.0;
   essential for the round-robin re-broadcaster to be safe.
7. **Hard WiFi reset** in `openEspNowRadio` (`esp_wifi_stop()` + 50 ms
   + `WiFi.mode(WIFI_AP_STA)` + 50 ms) — without this,
   `esp_wifi_set_channel(6)` was silently dropped on one of the two
   devices in 1.0.0.
8. **Diagnostic logs**: `register_recv_cb=` result,
   `esp_wifi_set_channel=` result + readback, `rxCb` counter in the
   Sending heartbeat. **Keep them.** The next time something
   regresses, those logs are how you'll know — they're the entire
   reason 1.0.1 could be diagnosed at all.

If you change anything in the friends path (`src/net.cpp` lines
~1000–1700), ask yourself: does this still survive directional RF
asymmetry **and** ESP32 ↔ ESP32-S3 cross-target? Test with at least
two different chip families simultaneously (e.g. Core2 + CoreS3) with
`pio device monitor` running on both ports — the 1.0.1 bugs only
showed up across chip families.

---

## Web radio — I2S contention with M5.Speaker

The MP3 decoder ([`src/webradio.cpp`](src/webradio.cpp)) and `M5.Speaker` (used for WAV sounds) share the I2S peripheral. Sequence in `webradio::start`:

1. `M5.Speaker.end()` — release I2S
2. Read pin config from `M5.Speaker.config()`
3. Set I2S pins on the audio decoder
4. Start the stream

In `webradio::stop`, the order is reversed: stop decoder → re-claim I2S via `M5.Speaker.begin()`.

If you forget either step, the device either makes no sound at all (M5.Speaker still owns I2S) or you double-claim and crash. The current sequence works — don't reorder without a reason.

While the radio is playing, [`src/voice_pipeline.cpp`](src/voice_pipeline.cpp) `pause()` mutes the wake/transcribe callbacks so Whisper doesn't transcribe the speaker output and trigger ghost tags.

---

## Module-LLM (Muffin only) — Qwen3 quirks

See [`docs/setup-muffin.md`](docs/setup-muffin.md) for the full Module-LLM workflow (ADB, .deb installs, framework upgrade). Two non-obvious behaviours that bit us:

- **Qwen3 thinking mode**: the model emits a `<think>...</think>` block by default. With small `max_token_len`, the answer is truncated mid-thought and never reaches the tag. Workarounds in the firmware: `cfg.max_token_len = 64`, suffix user input with `" /no_think"`, strip `<think>...</think>` before keyword matching.
- **Whisper-first bypass**: about 80 % of common commands match a German keyword directly on the Whisper transcript. Skip the LLM entirely for those — saves ~1–2 s and avoids the Qwen3 hallucinations (date spam, code blocks). LLM is fallback only.

If you touch the voice pipeline, log `work_id` after each `setup()`. Success returns `<unit>.<id>` (e.g. `whisper.1003`); failure returns the bare unit name (`whisper`). Easy to miss in passing.

---

## GitHub Pages site (under `site/`)

Static, deployed via GitHub Actions on push.

### Bilingual data attributes

Pages strings are bilingual — `data-en="..." data-de="..."` on the element, JS swaps the text content. Pitfall:

```html
<!-- BREAKS — ASCII " inside the German attribute closes it early -->
<p data-de="Das ist „kaputt"">…</p>

<!-- WORKS — German curly close quote " (U+201C), not ASCII " -->
<p data-de="Das ist „korrekt“">…</p>
```

Spent debugging time on this once (commit `2915e55`). Always verify with `grep` for `data-de=".*„.*"[^>]` after editing — proper Unicode close quotes will not match.

### Pet card "flagship" treatment

The Muffin card has a `pet-card-flagship` class that renders an accent border + ribbon. The voice-control feature card uses `feature-flagship` for matching emphasis. Don't drop these classes when editing the cards.

---

## Commit conventions

- **One commit = one logical change.** Don't bundle "fix typo + add feature + refactor". Easier review, easier revert.
- **Trailer**: every commit ends with `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>` (or whichever model wrote it). Use a HEREDOC for the message:

  ```bash
  git commit -m "$(cat <<'EOF'
  short title in present tense (no "fix:" prefix)

  Body explaining the why. Reference file paths if helpful.

  Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
  EOF
  )"
  ```

- **Stage explicit files** (`git add src/main.cpp site/index.html`), not `git add -A`. Avoids accidentally committing `.claude/`, `pkgs/*.deb`, `wifi_config.h`, etc.
- **Don't amend published commits.** Always make a new commit. The cost of an extra commit is much lower than confusing reviewers.
- **Pre-commit hooks**: never `--no-verify`. If a hook fails, fix the underlying issue.

---

## Workflow rules

### Confirm before risky actions

The user is the source of truth for "is this destructive enough to confirm?". Default to confirmation for:

- `git push --force` (especially to `main` / `master`)
- `git reset --hard`, `git clean -fd`
- Deleting branches, tags, files
- Pushing changes that affect a deployed surface (e.g. GitHub Pages)
- Installing or upgrading dependencies that shift the supply chain

The user is in Germany. Default response language follows the user's input language — usually German for this user, switch to English if the user does.

### Diagnose before patch

When a bug looks like it could be hardware/RF/timing, **add diagnostic logging first**, run, analyse, *then* patch. The friends-mode RF asymmetry took six iterations precisely because each step generated logs that ruled out hypotheses. Don't skip the logs to "save time" — you'll waste more.

### Subagents for big mechanical tasks

Use the `general-purpose` agent for large, repetitive, file-by-file work — e.g. translating comments across 20+ files, renaming an identifier project-wide, sweeping a deprecated API. Brief the subagent with a self-contained prompt (it has no chat context) and ask for a structured report back. Saves the main thread's context window.

Don't use a subagent for narrow lookups — `Grep`/`Read` directly is faster.

### Build all four after non-trivial code changes

Single-target builds miss capability-flag breakage. The CI matrix is `cores3 / core2 / visu / pip` — at minimum match it locally. Pip-S3 is a separate env (`pip-s3`); don't forget it if you touch StickC code.

### Hardware testing for hardware-coupled features

Anything that involves the radio, the IMU, the camera, the speaker, or the LLM module needs a hardware run before you call it done. Friends mode specifically needs **both** pets running simultaneously with serial monitors on each — single-pet testing won't catch directional asymmetry.

If you can't test on hardware, say so explicitly in your reply rather than claiming success.

### Don't overwrite plan / private dirs

`.claude/` (or your equivalent agent state dir) is gitignored and local-only. Don't add or commit it. Likewise `pkgs/*.deb`, `wifi_config.h`, screenshots in `tools/screenshots/raw/`, etc. — `.gitignore` is the source of truth.

---

## Common pitfalls (a short list)

| Pitfall | Where | What to do |
|---|---|---|
| `data-de` attribute breaks at first ASCII `"` after `„` | `site/index.html` | use `"` (U+201C) instead of `"` |
| Comment in German | anywhere in `src/` | translate to English |
| Capability flag check missing on a `#include` | feature `.cpp` and call site in `main.cpp` | wrap both in `#if TARGET_HAS_<flag>` |
| WiFi.setSleep(false) silently fails after esp_wifi_stop | net.cpp friendsBegin | use `esp_wifi_set_ps(WIFI_PS_NONE)` + log result |
| Single-target build hides another target's break | local dev | run all four envs (or at least core2 + cores3 + pip-s3) |
| Friends item lost despite RF working | net.cpp Sending | confirm BGN protocol + AP_STA in `openEspNowRadio` (NOT LR — see Friends section); also check the unicast peer was added when the partner MAC was first seen |
| Muffin / Visu sees `rxCb=0` while partner sends | net.cpp `openEspNowRadio` | mode must be `WIFI_AP_STA`, not `WIFI_STA`. CoreS3 stack drops broadcasts in disconnected-STA mode |
| Pip-S3 disappears from USB after deep sleep | main_pip.cpp `enterDeepSleep` | confirm the `#if CONFIG_IDF_TARGET_ESP32S3` block does `Serial.end() + delay(100)` BEFORE `M5.Power.deepSleep()` |
| StickC-S3 won't flash, "Write timeout" | hardware | hold BtnA + reset to enter download mode (see [`docs/setup-pip.md`](docs/setup-pip.md)) |
| `pio device monitor` holds the COM port | local dev | close monitor before flashing the same port |

---

## What NOT to do

- ❌ Add German comments back into `src/`
- ❌ Bundle multiple unrelated changes into one commit
- ❌ Push directly to `main` / `master` without explicit user instruction
- ❌ Use `--no-verify` to bypass hooks
- ❌ Skip building the other three targets after touching `main.cpp` or `face.cpp`
- ❌ Modify `i18n.cpp` German UI strings (those are user-facing, not comments)
- ❌ Disable LR mode in `friendsBegin` to "simplify" (it's load-bearing)
- ❌ Add a feature flag where the existing capability flags already cover the case
- ❌ Refactor working code for style alone

---

## Where to look first

| You want to … | Start at |
|---|---|
| understand the gameplay | [`docs/concept.md`](docs/concept.md) |
| understand the codebase layout | [`docs/architecture.md`](docs/architecture.md) |
| flash a specific pet | [`docs/hardware.md`](docs/hardware.md) → per-pet guide ([goo-goo](docs/setup-goo-goo.md) / [visu](docs/setup-visu.md) / [pip](docs/setup-pip.md) / [muffin](docs/setup-muffin.md)) |
| set up the Module-LLM | [`docs/setup-muffin.md`](docs/setup-muffin.md) |
| add a new sound | [`docs/sound_assets.md`](docs/sound_assets.md), [`src/sounds/`](src/sounds/) |
| change a UI string | [`src/i18n.h`](src/i18n.h) + [`src/i18n.cpp`](src/i18n.cpp) (both!) |
| add a feature gate | [`src/target_caps.h`](src/target_caps.h) |
| change pet logic | [`src/main.cpp`](src/main.cpp) (or [`src/main_pip.cpp`](src/main_pip.cpp) for pip) |
| change the renderer | [`src/face.cpp`](src/face.cpp) — read `PetView` only, never `g_pet` |
| change the marketing site | [`site/index.html`](site/index.html), [`site/styles.css`](site/styles.css) |

---

That's the floor. The walls are in the docs, the ceiling is the user's judgement.
