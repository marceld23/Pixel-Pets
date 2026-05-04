# Setup: Muffin (CoreS3 + Module-LLM)

**Difficulty: 🔴 Advanced.** Hardest setup of the four. You're flashing TWO devices (the CoreS3 sketch via USB-C, and the Module-LLM via a separate USB-C and ADB), then configuring software on the LLM module's Linux. Plan an evening.

If this feels like too much, **start with [Visu](setup-visu.md) instead** — same CoreS3 hardware, no Module-LLM, no Linux setup. You can add the Module-LLM later by following Step 2 of this guide.

## What you need

- **M5Stack CoreS3** ([store link](https://shop.m5stack.com/products/m5stack-cores3-esp32s3-iot-development-kit))
- **M5Stack Module LLM (M140 / AX630C)** ([store link](https://shop.m5stack.com/products/m5stack-llm-large-language-model-module-kit-ax630c)) — 4 GB RAM, 32 GB eMMC, runs Ubuntu 22.04 aarch64
- **Battery Module 13.2** ([store link](https://shop.m5stack.com/products/battery-module-13-2-1500mah)) — 1500 mAh, separate purchase from the LLM module bundle
- A **USB-C cable** (you'll need to swap it between the CoreS3 and the LLM module)
- A computer with [PlatformIO](https://platformio.org/) installed
- **ADB** on the same computer (`winget install Google.PlatformTools` on Windows; `brew install android-platform-tools` on macOS; `apt install adb` on Linux)

## Hardware components

| Device | Role | SoC | OS |
|---|---|---|---|
| CoreS3 | Host / UI / UART master | ESP32-S3 | Arduino framework (PlatformIO) |
| Module-LLM (M140) | AI inference (ASR / LLM / TTS / KWS / VAD) | Axera AX620E | Ubuntu 22.04 LTS aarch64 |

Connected via the **9-pin bus (Port C)**: UART2 on the CoreS3, 115200 baud, 8N1. Pin mapping (CoreS3 side):

- `RX = GPIO 18` → `M5.getPin(m5::pin_name_t::port_c_rxd)`
- `TX = GPIO 17` → `M5.getPin(m5::pin_name_t::port_c_txd)`

Stack physically: Battery Module 13.2 at the bottom, CoreS3 in the middle, Module-LLM on top.

---

## Step 1: Flash the CoreS3 sketch

Same idea as [Visu setup](setup-visu.md), just with the `cores3` env (which includes the voice pipeline):

```bash
pio device list                                           # find the CoreS3 (303A:1001)
pio run -e cores3 -t upload --upload-port COM<N>
```

Build takes ~2–3 minutes (longer than other targets because of the M5Module-LLM library + voice pipeline). The CoreS3 boots; if no Module-LLM is configured yet you'll see the splash and pet, but the wake word won't respond.

Serial monitor:

```bash
pio device monitor -p COM<N> -b 115200
```

The relevant `[env:cores3]` section in [`../platformio.ini`](../platformio.ini) pulls the board, lib_deps (incl. `M5Module-LLM`) and partition CSV automatically — no manual setup.

---

## Step 2: Configure the Module-LLM

The factory image of the Module-LLM ships with `lib-llm 1.3` and `qwen2.5-0.5B-prefill-20e` as the only LLM. Pixel Pets needs Whisper-Base, VAD and Qwen3-0.6B, plus a few service tweaks. This is one-time work.

### 2a. Connect via ADB

The Module-LLM has its **own USB-C port** (on the side of the module, not the CoreS3's). With one cable, swap it between the CoreS3 and the Module-LLM as needed.

Plug the cable into the **module** now. Verify:

```bash
adb devices
# expected: axera-ax620e   device
```

If nothing shows up, check Device Manager (Windows) for `AX620B-ADB`. ADB is root by default — no password.

### 2b. Push the package set + install

All required `.deb` files are pre-staged under [`../pkgs/`](../pkgs/) in this repo. Run from the repo root:

```bash
# Path to the `adb` executable. If it's already on your PATH (`adb` works
# in your shell), leave this as `adb`. Otherwise point it at your install:
#   Windows:  winget install Google.PlatformTools  (then `adb` is on PATH after a new shell)
#   macOS:    brew install android-platform-tools
#   Linux:    apt install adb  (or distro equivalent)
ADB=adb

cd pkgs

# 1) Upload all packages — they end up flat under /root/pkgs/, dpkg works
#    with the bare filenames afterwards.
for f in framework/lib-llm_1.8-m5stack1_arm64.deb \
         framework/llm-sys_1.6-m5stack1_arm64.deb \
         framework/llm-llm_1.8-m5stack1_arm64.deb \
         services/llm-vad_1.5.deb \
         services/llm-whisper_1.5.deb \
         models/llm-model-silero-vad_0.4.deb \
         models/llm-model-whisper-base_0.4.deb \
         models/llm-model-qwen3-0.6B-ax630c_0.4.deb \
         audio/silent_wakeup.wav; do
    MSYS_NO_PATHCONV=1 "$ADB" push "$f" /root/pkgs/
done

# 2) Framework upgrade (lib-llm + llm-sys + llm-llm)
MSYS_NO_PATHCONV=1 "$ADB" shell "cd /root/pkgs && \
    dpkg -i lib-llm_1.8-m5stack1_arm64.deb && \
    dpkg -i llm-sys_1.6-m5stack1_arm64.deb && \
    dpkg -i llm-llm_1.8-m5stack1_arm64.deb && \
    systemctl daemon-reload && \
    systemctl restart llm-sys llm-llm"

# 3) Whisper + VAD service binaries + their model files
MSYS_NO_PATHCONV=1 "$ADB" shell "cd /root/pkgs && \
    dpkg -i llm-vad_1.5.deb && \
    dpkg -i llm-whisper_1.5.deb && \
    dpkg -i --force-depends llm-model-silero-vad_0.4.deb && \
    dpkg -i --force-depends llm-model-whisper-base_0.4.deb && \
    systemctl daemon-reload && \
    systemctl start llm-vad llm-whisper"

# 4) Qwen3 LLM model
MSYS_NO_PATHCONV=1 "$ADB" shell "dpkg -i --force-depends /root/pkgs/llm-model-qwen3-0.6B-ax630c_0.4.deb"

# 5) Permanently disable TTS services (CoreS3 plays its own sounds)
MSYS_NO_PATHCONV=1 "$ADB" shell "systemctl stop llm-tts llm-melotts; \
    systemctl disable llm-tts llm-melotts"

# 6) Silence the default wake-up WAV (pre-recorded "Hi" voice)
MSYS_NO_PATHCONV=1 "$ADB" shell "cp /opt/m5stack/data/audio/wakeup_en_us.wav /opt/m5stack/data/audio/wakeup_en_us.wav.bak"
MSYS_NO_PATHCONV=1 "$ADB" push audio/silent_wakeup.wav /opt/m5stack/data/audio/wakeup_en_us.wav

# 7) Verify
MSYS_NO_PATHCONV=1 "$ADB" shell "systemctl is-active llm-sys llm-llm llm-audio llm-kws llm-vad llm-whisper; \
    dpkg -l | grep -E 'lib-llm|llm-llm|llm-sys|llm-vad|llm-whisper|qwen3' | awk '{print \$2, \$3}'"
```

The last command should print all six services as `active` and the package versions you just installed.

### 2c. Plug the USB-C back into the CoreS3

The Module-LLM is now ready. Plug the cable back into the CoreS3 and use the pet normally — call **"Muffin!"** and it should wake up.

---

## What the firmware sends to the module at runtime

The packages above only decide what the module is *capable of*. Which model, language and wake word are actually **used** comes from the CoreS3 firmware over UART on every boot, defined in [`../src/voice_pipeline.h`](../src/voice_pipeline.h):

| Firmware field | Current value | Meaning |
|---|---|---|
| `wake_word` | `"MUFFIN"` | What the KWS listens for. Sherpa-KWS supports arbitrary words; just change the string. |
| `whisper_language` | follows UI language | Resolved at boot from `g_pet.persisted.language` (`0` → `"de"`, `1` → `"en"`) in `voiceSetupTask`. Whisper-base is multilingual; switching the UI language and rebooting is enough. |
| `whisper_model` | `"whisper-base"` | Maps to the `.deb` filename without the version: `llm-model-whisper-base_*.deb` → `whisper-base`. Switching to `"whisper-tiny"` works if the corresponding model is installed. |
| `llm_model` | `"qwen3-0.6B-ax630c"` | Same naming rule. The 1.5B-Int4 identifier loads on paper but the package is broken — see the gotcha below. |
| `llm_max_tokens` | `64` | Hard cap; raising it doesn't help with Qwen3 thinking-mode (see below). |
| `mic_volume` | `0.7` | Capture-side gain, sent to `audio.setup`. |

The **system prompt** lives in [`../src/main.cpp`](../src/main.cpp) at the top, in the `SYSTEM_PROMPT` const. It frames Qwen3 as a tag classifier: input is a German or English sentence, output is 1–3 lowercase tags from a closed list of 23 (`eat`, `pet`, `love`, `laugh`, `sleep`, `wake`, `greet`, `sad`, `startle`, `sing`, `dance`, `ball`, `mouse`, `rattle`, `butterfly`, `plush`, `movie`, `game`, `internet`, `social`, `friends`, `radio`, `idle`). The closed-vocabulary framing is what keeps the 0.6B model usable — open-ended generation hallucinates code or invents German on unclear input.

### Worked example: switch Whisper to English

Settings → Sprache → English on the device, then reboot. `voiceSetupTask` reads `g_pet.persisted.language` on the next boot and sends `whisper_language = "en"` over UART. No firmware edit, no module change.

To pin Whisper to English regardless of UI language, override the line in `voiceSetupTask` ([`../src/main.cpp`](../src/main.cpp)):

```cpp
vcfg.whisper_language = "en";   // hard-pin instead of following persisted.language
```

The system prompt already handles both languages; no prompt change needed when ASR switches.

---

## Gotchas

### Qwen3 thinking mode

Qwen3 has chain-of-thought reasoning ON by default. Without intervention, every classification call generates a `<think>...</think>` block before the answer:

```
<think>
The user said "Hier hast du Futter" which means feeding the pet...
</think>
[EAT]
```

That wastes tokens (and with `max_token_len=64` the answer can be cut off mid-thought).

**Workarounds in the firmware:**

- `cfg.max_token_len = 64` — enough headroom for an empty `<think></think>` (with `/no_think`) plus three tags.
- User input is suffixed with `" /no_think"` — a Qwen3 soft switch that suppresses thinking (doesn't always hit 100 %).
- The tag parser strips `<think>...</think>` blocks before keyword matching; if `<think>` appears without a closing tag, returns IDLE.

**Heavier solution** (not implemented but documented): edit `/opt/m5stack/scripts/tokenizer_qwen3-0.6B-ax630c.py` and pass `enable_thinking=False` to `apply_chat_template`:

```python
text = self.tokenizer.apply_chat_template(
    messages,
    tokenize=False,
    add_generation_prompt=True,
    enable_thinking=False,
)
```

Plug USB into the module, edit the file, kill the tokenizer subprocess (auto-respawned by `llm-llm`).

### Qwen2.5-1.5B-Int4 doesn't load (tokenizer missing)

The package `llm-model-qwen2.5-1.5B-Int4-ax630c v0.4` is incomplete — it lacks the `tokenizer/` subdirectory.

Comparison:

- `/opt/m5stack/data/qwen3-0.6B-ax630c/tokenizer/` contains `tokenizer.json`, `tokenizer_config.json`, `vocab.json`, `merges.txt`, `config.json` ✅
- `/opt/m5stack/data/qwen2.5-1.5B-Int4-ax630c/` has **no** `tokenizer/` subdirectory ❌

Pixel Pets uses Qwen3-0.6B for that reason. If you want to try the larger model, download tokenizer files from [HuggingFace Qwen2.5-1.5B-Instruct-GPTQ-Int4](https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GPTQ-Int4) yourself, push them to `/opt/m5stack/data/qwen2.5-1.5B-Int4-ax630c/tokenizer/`, adjust the tokenizer script `--model_id`, restart the service.

### Wake confirmation plays a WAV file

The module plays a pre-recorded `wakeup_en_us.wav` on every wake event — independent of the TTS engine, independent of `enkws` flags. Step 2b above replaces it with a silent 50 ms WAV (kept as `wakeup_en_us.wav.bak`). The CoreS3 firmware plays its own `Sound::Wake` instead.

The wake confirmation plays on the **LLM module's speaker**. The CoreS3 has its own speaker and can independently play a beep via `M5.Speaker.tone()` — see `playWakeBeep()` in the sketch.

### `llm-melotts` produces unwanted Chinese audio

If you only use the `tts` API (not `melotts`), the parallel `llm-melotts` service still reacts to wake events and plays a default greeting in the voice of the most recently loaded model (Chinese, by default). Step 2b's `systemctl disable llm-melotts` handles this.

### MeloTTS model selection on `lib-llm < 1.6`

In `api_melotts.cpp` of the M5Module-LLM library, the resolution `language → model` depends on the framework version:

```cpp
if (version >= 1.6) {
    // language → "melotts-zh-cn" / "melotts-ja-jp" / "melotts-en-default"
} else {
    if (language == "zh_CN") doc["data"]["model"] = "melotts_zh-cn";
    // otherwise: stays config.model (default: "melotts_zh-cn")
}
```

On the factory `lib-llm 1.3`, the default `"melotts_zh-cn"` for any language ≠ `zh_CN` is **not** overridden — TTS sounds Chinese even though `language` is set correctly. We avoid this by upgrading to `lib-llm 1.8` (Step 2b) and disabling MeloTTS entirely.

### TTS streaming aborts after the first response (v1.3 framework)

When chaining `llm_work_id → tts.utf-8.stream` via `cfg.input = {"tts.utf-8.stream", llm_work_id}`, the subscription on `llm-tts 1.3` is one-shot: after the first response's `finish:true`, no audio comes for the second.

Sketch-side workaround: do NOT chain TTS to the LLM stream — instead collect the full LLM response text in a buffer and synthesise it explicitly via `module_llm.tts.inference(tts_work_id, text)` after `finish:true`. Pixel Pets doesn't use the module's TTS anyway (we disabled it in Step 2b), but worth knowing if you reactivate it.

### Git Bash MSYS path conversion

Git Bash on Windows auto-converts Unix paths to Windows paths. `adb push file.deb /root/pkgs/` becomes `adb push file.deb C:/Program Files/Git/root/pkgs/`. The `MSYS_NO_PATHCONV=1` prefix in Step 2b avoids that. In regular PowerShell or CMD it's not needed.

### COM port held by serial monitor

Same as Visu: while `pio device monitor` is open on the CoreS3 port, `pio run -t upload` fails with `Could not open COM<N>`. Close the monitor before flashing.

---

## Verification: did a service setup actually succeed?

In the Arduino M5Module-LLM API, every `setup()` returns a `work_id`:

- **Success**: `<unit>.<id>`, e.g. `whisper.1003`, `vad.1002`
- **Failure** (model missing, service not installed): just the bare unit name, e.g. `whisper`, `vad`

When debugging, log `work_id` and watch for the `.NNNN` suffix.

---

## Reference: current state of the Module-LLM (April 2026)

After Step 2b, the module's state is:

### systemd services

| Service | Status | Note |
|---|---|---|
| `llm-sys` | active | StackFlow coordinator |
| `llm-audio` | active | microphone capture, speaker |
| `llm-kws` | active | wake-word detection (Sherpa-ONNX-KWS-Zipformer-Gigaspeech) |
| `llm-vad` | active | voice activity detection (Silero-VAD) |
| `llm-whisper` | active | multi-language ASR (Whisper-base, configured for German by default) |
| `llm-llm` | active | LLM inference (Qwen3-0.6B) |
| `llm-tts` | **disabled** | silent in the pet use case |
| `llm-melotts` | **disabled** | silent in the pet use case |
| `llm-camera`, `llm-vlm`, `llm-yolo`, `llm-skel` | active | unused, could be disabled to free RAM |

### Installed model packages

| Package | Version | Use |
|---|---|---|
| `lib-llm` | 1.8 | framework library (raised from factory 1.3) |
| `llm-sys` | 1.6 | coordinator (raised from 1.3, deps lib-llm ≥ 1.7) |
| `llm-llm` | 1.8 | LLM service (raised from 1.3, deps lib-llm ≥ 1.7) — speaks Qwen3 |
| `llm-whisper` | 1.5 | Whisper service |
| `llm-vad` | 1.5 | VAD service |
| `llm-model-whisper-base` | 0.4 | Whisper-Base, multilingual (~300 MB, ~2 s latency) |
| `llm-model-silero-vad` | 0.4 | Silero-VAD model |
| `llm-model-qwen3-0.6B-ax630c` | 0.4 | currently used |

Factory models still on the module (`qwen2.5-0.5B-prefill-20e`, KWS models, Sherpa-ASR) remain untouched.

---

## Reference: ADB workflow + module access

### Important paths on the module

- `/opt/m5stack/bin/` — service binaries (`llm_asr`, `llm_kws`, `llm_llm`, `llm_melotts`, `llm_whisper`, `llm_vad`, …)
- `/opt/m5stack/data/` — model files (e.g. `whisper-base/`, `melotts-en-us/`, `silero-vad/`)
- `/lib/systemd/system/llm-*.service` — systemd units per component
- `/etc/apt/sources.list.d/` — APT sources
- `/var/lib/dpkg/status` — installed packages

### Shell access

```bash
adb shell
# default user: root, no password
# default SSH credentials (if Ethernet is available): root / 123456
```

### When `--force-depends` is OK

The factory ships `lib-llm 1.3`. Model packages declare `Depends: lib-llm (>= 1.6)` but contain only data files (`.axmodel`, `.ort`, `.json`) — no libraries with ABI binding. In that case `--force-depends` is safe.

Service binaries (`llm-whisper`, `llm-vad` v1.5) only declare `Depends: lib-llm` without a version constraint, so they install cleanly against older `lib-llm` too.

**Rule of thumb:** before forcing, run `dpkg-deb -c <package>.deb` and check that only data files are inside. For `.so` files or version-constrained binaries, do **not** force — instead upgrade the framework and dependents together.

### Network on the module (it doesn't have any by default)

- `eth0` is `NO-CARRIER` (no RJ45 jack on the M140 itself)
- No WiFi
- ADB is USB-data only, no TCP/IP

To bring the module online for live `apt` updates: M5Stack Debug board accessory (RJ45) or `adb reverse` HTTP proxy on the PC. Otherwise stick with the offline workflow above.

---

## Reference: M5Stack APT repository

If you want to download fresh `.deb` files instead of using the staged ones in `pkgs/`:

- Repo URL: `https://repo.llm.m5stack.com/m5stack-apt-repo`
- Suite: `jammy`
- Component: `ax630c`
- Architecture: `arm64`
- Documentation: https://docs.m5stack.com/en/stackflow/module_llm/software

Fetch the `Packages` index to see filenames + dependencies:

```bash
curl -O https://repo.llm.m5stack.com/m5stack-apt-repo/dists/jammy/ax630c/binary-arm64/Packages
```

Download a specific file:

```bash
curl -fsSL -o llm-whisper_1.5.deb \
  "https://repo.llm.m5stack.com/m5stack-apt-repo/pool/jammy/ax630c/v1.5/llm-whisper_1.5-m5stack1_arm64.deb"
```

---

## Reference: available models on the module repo

State of the M5Stack repo research (April 2026):

| Component | Packages | Languages |
|---|---|---|
| Whisper | `llm-model-whisper-tiny`, `-base`, `-small` | multilingual (incl. German) |
| VAD | `llm-model-silero-vad` | language-agnostic |
| Sherpa-ASR (streaming) | `llm-model-sherpa-ncnn-streaming-zipformer-{20M-en,zh-14M}`, `…-bilingual-zh-en` | en, zh, en+zh |
| KWS | `llm-model-sherpa-onnx-kws-zipformer-{gigaspeech,wenetspeech}` | en, zh |
| MeloTTS | `llm-model-melotts-{en-us,en-au,en-br,en-india,en-default,es-es,ja-jp,zh-cn}` | **no German** |
| TTS (legacy) | `llm-model-single-speaker-{english-fast,fast}` | en, zh |
| LLM | `llm-model-qwen2.5-{0.5B,1.5B}`, `llm-model-qwen3-0.6B`, `llm-model-llama3.2-1B`, `llm-model-deepseek-r1-1.5B`, `llm-model-internvl2.5-1B`, `llm-model-smolvlm-256M/500M`, … | multilingual (model-dependent) |

**Consequences for the pet-controller setup (final):**

- ASR in German: ✅ via Whisper-base with `language="de"`
- LLM classification: ✅ Qwen3-0.6B with English prompt + German keyword hints (`lieb=love`)
- TTS on the LLM module: **disabled** — the CoreS3 has its own sounds
- Wake confirmation WAV: replaced with silence — pet plays its own `Sound::Wake` on the CoreS3 speaker

---

## Reference: chronological log of what's been done to this module

What was done to this module beyond the factory image — kept here so a later wipe / re-flash can replicate the steps in order.

| # | Action | Command / path |
|---|---|---|
| 1 | Installed ADB on the PC | `winget install Google.PlatformTools` |
| 2 | Installed Whisper and VAD packages on the module (factory had neither) | `dpkg -i llm-vad_1.5.deb llm-whisper_1.5.deb`, then `dpkg -i --force-depends llm-model-silero-vad_0.4.deb llm-model-whisper-tiny_0.4.deb` |
| 3 | Disabled `llm-melotts` (was producing the unwanted Chinese wake greeting) | `systemctl stop llm-melotts && systemctl disable llm-melotts` |
| 4 | Replaced `wakeup_en_us.wav` with the silent WAV | `adb push silent_wakeup.wav /opt/m5stack/data/audio/wakeup_en_us.wav` |
| 5 | Installed the English MeloTTS model (transitional, now unused) | `dpkg -i --force-depends llm-model-melotts-en-us_0.6.deb` |
| 6 | Installed the Whisper-Base model (better than tiny) | `dpkg -i --force-depends llm-model-whisper-base_0.4.deb` |
| 7 | Disabled `llm-tts` (the pet doesn't need TTS) | `systemctl stop llm-tts && systemctl disable llm-tts` |
| 8 | Raised the framework stack (for Qwen3): `lib-llm 1.3 → 1.8`, `llm-sys 1.3 → 1.6`, `llm-llm 1.3 → 1.8` | `dpkg -i lib-llm_1.8.deb llm-sys_1.6.deb llm-llm_1.8.deb` |
| 9 | Installed the Qwen2.5-1.5B-Int4 model (broken, see gotcha) | `dpkg -i --force-depends llm-model-qwen2.5-1.5B-Int4-ax630c_0.4.deb` |
| 10 | Installed the Qwen3-0.6B model | `dpkg -i --force-depends llm-model-qwen3-0.6B-ax630c_0.4.deb` |
| 11 | Re-disabled TTS services after the lib-llm post-install (postinst had re-enabled them) | `systemctl stop llm-tts llm-melotts && systemctl disable llm-tts llm-melotts` |

The .deb files live under [`../pkgs/`](../pkgs/), sorted by function:

- [`../pkgs/framework/`](../pkgs/framework/) — `lib-llm`, `llm-sys`, `llm-llm` (core stack)
- [`../pkgs/services/`](../pkgs/services/) — `llm-vad`, `llm-whisper` (service binaries)
- [`../pkgs/models/`](../pkgs/models/) — all `llm-model-*` (model data)
- [`../pkgs/audio/`](../pkgs/audio/) — `silent_wakeup.wav` (the only tracked file; .debs gitignored)

---

## Workflow shortcuts

**To change module software:**

1. Unplug USB-C from the CoreS3, plug into the LLM module
2. `adb devices` (should show `axera-ax620e`)
3. Push `.deb`s + `dpkg -i` + restart the service
4. Plug USB-C back into the CoreS3

**To change the sketch:**

1. Close the serial monitor if it's open
2. USB-C on the CoreS3 (if not already)
3. `pio run -e cores3 -t upload --upload-port COM<N>`
4. Optional: reopen the monitor
