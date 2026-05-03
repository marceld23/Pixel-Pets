# Pixel Pets — hardware setup

Notes on setting up, flashing and updating the four supported build targets.

## Build targets

The repo has four targets, selected via `pio run -e <env>`:

| Target | Board | SoC | LLM | Camera | Hard buttons | Display | Audio | Branding |
|---|---|---|---|---|---|---|---|---|
| `cores3` | M5Stack CoreS3 + Module-LLM | ESP32-S3 | ✅ | ✅ | ❌ | 320×240 | WAV | Muffin |
| `visu`   | M5Stack CoreS3 without module | ESP32-S3 | ❌ | ✅ | ❌ | 320×240 | WAV | Visu |
| `core2`  | M5Stack Core2               | ESP32    | ❌ | ❌ | ✅ BtnA/B/C | 320×240 | WAV | Goo-Goo |
| `pip`    | M5StickC Plus 2 (ESP32 PICO)  | ESP32    | ❌ | ❌ | ✅ BtnA/B   | 135×240 | Buzzer | Pip |
| `pip-s3` | M5StickC Plus 2 (S3 revision) | ESP32-S3 | ❌ | ❌ | ✅ BtnA/B   | 135×240 | Buzzer | Pip |

Capabilities are derived in [`../src/target_caps.h`](../src/target_caps.h) from the `TARGET_*` build flags:

- `TARGET_HAS_LLM`         → voice pipeline (KWS/VAD/Whisper/Qwen3)
- `TARGET_HAS_CAMERA`      → front camera + face detection
- `TARGET_HAS_HARD_BUTTONS`→ physical BtnA/B/C
- `TARGET_HAS_TOUCH`       → touchscreen
- `TARGET_HAS_WAV_AUDIO`   → WAV speaker (otherwise PWM buzzer)
- `TARGET_HAS_WIFI`        → active WiFi (NTP, ip-api, web radio, ESP-NOW friends, captive portal)
- `TARGET_DISPLAY_W` / `_H`→ display dimensions
- `TARGET_NAME` / `TARGET_AP_NAME` / `TARGET_MDNS_NAME` → branding

Default env: `cores3`. Build and flash commands:

```bash
pio run -e cores3                                      # build (CoreS3 + LLM)
pio run -e cores3 -t upload --upload-port COM5         # build + flash (CoreS3)
pio run -e visu                                        # CoreS3 hardware without voice
pio run -e core2                                       # Core2
pio run -e pip                                         # StickC Plus 2
pio run -e cores3 -e core2 -e visu -e pip              # build all
```

**Visu** and **Pip** were added in April 2026:

- Visu = exactly the same CoreS3 hardware as Muffin, but without an attached Module-LLM. The voice pipeline is excluded via `build_src_filter -<voice_pipeline.cpp>`, all voice calls in `main.cpp` are gated behind `#if TARGET_HAS_LLM`. Front camera + touch remain.
- Pip = M5StickC Plus 2, a small mobile pet with buzzer instead of WAV, BtnA/B instead of touch, and a dedicated 135×240 renderer in `src/pip/`. Very reduced, with its own `main_pip.cpp`.

The rest of this document is primarily **CoreS3 + Module-LLM**-specific (voice pipeline setup, Qwen3 quirks, module ADB workflow). Core2 is intentionally minimalistic — no module, no camera, no extra setup — see the next section.

---

## M5Stack Core2 (`core2` target)

Significantly simpler than the CoreS3 setup:

- **USB connection**: Core2 has a CH9102 USB bridge on the device's underside. It appears under Windows as a COM port with a different VID/PID than the CoreS3.
- **Input**: three physical buttons under the display (BtnA / BtnB / BtnC) for Greet / Feed / Sleep. The bottom hint strip on the LCD still draws the glyphs — on Core2 they serve as visual labels for the hardware buttons.
- **What's missing vs. CoreS3**: no wake word, no voice tags, no listening mimic, no face detection. The associated help pages (Voice / Camera) are skipped on Core2 in the help UI, but their i18n strings remain compiled in.
- **Branding**: splash shows "Goo-Goo", WiFi AP is `goo-goo-setup`, mDNS is `goo-goo.local`.
- **Pet logic identical**: all mini-games, foraging, ESP-NOW friends, weather, moon, scenes, sleep regen, gift bar — all work exactly as on CoreS3.

Partition table: [`../partitions_core2_16MB.csv`](../partitions_core2_16MB.csv). No Module-LLM = no extra setup work. Just flash and it runs.

---

## CoreS3 + Module-LLM — components

| Device | Role | SoC | OS |
|---|---|---|---|
| **M5Stack CoreS3** | Host / UI / UART master | ESP32-S3 | Arduino framework (PlatformIO) |
| **M5Stack Module-LLM (M140)** | AI inference (ASR/LLM/TTS/KWS/VAD) | Axera AX620E, 4 GB RAM, 32 GB eMMC | Ubuntu 22.04 LTS aarch64 |

Connected via the **9-pin bus (Port C)**: UART2 on the CoreS3, 115200 baud, 8N1.
Pin mapping (CoreS3 side):

- `RX = GPIO 18` → `M5.getPin(m5::pin_name_t::port_c_rxd)`
- `TX = GPIO 17` → `M5.getPin(m5::pin_name_t::port_c_txd)`

---

## CoreS3 — flashing and monitoring

### USB connection

CoreS3 has its own USB-C port that appears under Windows as a **COM port** (Espressif USB-CDC, VID:PID `303A:1001`). In this setup: `COM5`.

```bash
pio device list                # lists available COM ports
```

### Build & flash (PlatformIO)

```bash
# build only
pio run -e cores3

# build + flash
pio run -e cores3 -t upload --upload-port COM5
```

The relevant `[env:cores3]` section in [`../platformio.ini`](../platformio.ini) pulls the board, lib_deps (incl. `M5Module-LLM`) and partition CSV automatically — no manual setup needed.

### Serial monitor

In VSCode: **plug-icon at the bottom of the status bar**, or in the terminal:

```bash
pio device monitor -p COM5 -b 115200
```

Exit with `Ctrl+C`.

### Gotcha: COM port conflicts

While the serial monitor is open, `COM5` is held **exclusively** and an upload fails with `Could not open COM5`. Close the monitor before each flash.

---

## M5StickC Plus 2 (S3 revision) — flashing

The newer S3 revision of the StickC Plus 2 ships with native ESP32-S3 USB-CDC and has a different download-mode behaviour from the older Plus 2 (CH9102 bridge):

- **Normal mode**: appears as one COM port, VID:PID `303A:8120` (USB JTAG/serial debug unit).
- **Download mode**: re-enumerates as a different COM port, VID:PID `303A:1001` (USB-CDC).

The auto-reset-via-DTR/RTS that PlatformIO normally uses to put the chip into download mode does **not** work reliably on the S3 with native USB. Symptoms:

```
A serial exception error occurred: Write timeout
```

**Manual download-mode procedure** (verified on the actual hardware — earlier shorter variant didn't always take):

1. **Disconnect USB** completely.
2. **Hold BtnA** (the front button under the M5 logo) and keep holding.
3. While still holding BtnA, **press and hold the side reset button** for ~2 s.
4. Release the reset button, keep holding BtnA for ~1 more second, then release BtnA.

The display stays black — the chip is now in download mode. Plug USB back in if you disconnected it. Check `pio device list` and look for `303A:1001`.

After that, **re-check the COM port** because it changes between normal and download mode:

```bash
pio device list
```

Look for the entry with VID:PID `303A:1001` — that's the download-mode CDC port. Then flash with that port:

```bash
pio run -e pip-s3 -t upload --upload-port COM<N>
```

After a successful flash the device hard-resets via RTS pin and re-enumerates back on the original `303A:8120` port for normal use.

**Use `pio run -e pip-s3`** for the S3 revision. The original Plus 2 (ESP32-PICO + CH9102) still uses `pio run -e pip` with the regular auto-reset flow — no button gymnastics there.

### Recovery: stuck in ROM bootloader after flash

A frequent failure mode on the S3 stick: the flash succeeds, esptool prints `Hard resetting via RTS pin...`, but the chip stays in ROM bootloader (display black, slow green LED blink, COM port still `303A:1001`). The auto-reset is genuinely unreliable on native USB-CDC because the RTS pin isn't physically wired the way esptool assumes. Symptoms and recovery, in order of likelihood:

1. **Try the side reset button.** M5StickC PLUS2 has a tiny hardware reset button on the right edge (above BtnB). One short press → clean boot from flash.
2. **USB unplug → 5 s wait → plug back in.** No buttons held. The 5-second wait lets the residual capacitance drain so GPIO0 strapping is read fresh on power-up.
3. **Mechanically un-stick BtnA.** GPIO0 is the boot strap pin, and on the StickC PLUS2 it's wired to BtnA. After repeated download-mode entries, the BtnA microswitch can stick in a partial-press state and read low at boot — chip enters download mode every time. Click BtnA gently 20–30× in rapid succession to dislodge the switch, then unplug + replug USB.
4. **Drain the battery.** If everything else fails, leave the device unplugged for 1–2 hours. The 200 mAh battery discharges enough to fully reset the SoC. Slow but bulletproof.

### Avoid `-DARDUINO_USB_CDC_ON_BOOT=1` on the pip-s3 env

Tempting but harmful: this Arduino flag remaps `Serial` to the native USB-CDC so `pio device monitor` can read `Serial.printf` output. **Adding it to `[env:pip-s3]` causes the firmware to fail to leave the ROM bootloader after flash** — the auto-reset path gets even more confused than it already is. The flag is enabled briefly during the Phase 2 development cycle and then reverted (commit `f0ac653`). If you need on-Pip diagnostics, use ESP-IDF logging (`ESP_LOGE` / `esp_log_*`) which writes to USB-Serial/JTAG natively without the flag, or wire the device's UART pins to an external USB-TTL adapter.

---

## LLM module — access via ADB

### USB connection

The LLM module has its **own USB-C port** (on the side of the module, not via the CoreS3). When connected, it shows up on the PC as an **ADB device** (Android Debug Bridge over USB), appearing in Device Manager under "USB devices" as `AX620B-ADB`.

> **Important:** this USB port is **not** the CoreS3 USB-C port. With only one USB cable available, you have to swap the cable between the two devices depending on whether you want to flash the CoreS3 or administer the LLM module.

### Install ADB (Windows)

```bash
winget install Google.PlatformTools
```

Installs ADB to:

```
%LOCALAPPDATA%\Microsoft\WinGet\Packages\Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe\platform-tools\adb.exe
```

PATH is set, but **only takes effect in newly started shells**.

### Verify the connection

```bash
adb devices
# expected: axera-ax620e   device
```

### Shell access

```bash
adb shell
# default user: root, no password needed (ADB is root)
# default SSH credentials (if Ethernet is available): root / 123456
```

Important paths on the module:

- `/opt/m5stack/bin/` — service binaries (`llm_asr`, `llm_kws`, `llm_llm`, `llm_melotts`, `llm_whisper`, `llm_vad`, …)
- `/opt/m5stack/data/` — model files (e.g. `whisper-tiny/`, `melotts-en-us/`, `silero-vad/`)
- `/lib/systemd/system/llm-*.service` — systemd units per component
- `/etc/apt/sources.list.d/` — APT sources
- `/var/lib/dpkg/status` — installed packages

### Gotcha: Git Bash MSYS path conversion

Git Bash on Windows automatically converts Unix paths to Windows paths. `adb push file.deb /root/pkgs/` becomes `adb push file.deb C:/Program Files/Git/root/pkgs/`. Workaround:

```bash
MSYS_NO_PATHCONV=1 adb push file.deb /root/pkgs/
```

In a regular PowerShell or CMD this is not necessary.

---

## LLM module — software updates

### M5Stack APT repository

Official source for `lib-llm`, all `llm-*` service binaries and all `llm-model-*` model packages:

- **Repo URL**: `https://repo.llm.m5stack.com/m5stack-apt-repo`
- **Suite**: `jammy`
- **Component**: `ax630c`
- **Architecture**: `arm64`
- **Documentation**: https://docs.m5stack.com/en/stackflow/module_llm/software

### Add the repo (when the module has internet)

```bash
wget -qO /etc/apt/keyrings/StackFlow.gpg \
  https://repo.llm.m5stack.com/m5stack-apt-repo/key/StackFlow.gpg

echo 'deb [arch=arm64 signed-by=/etc/apt/keyrings/StackFlow.gpg] \
  https://repo.llm.m5stack.com/m5stack-apt-repo jammy ax630c' \
  > /etc/apt/sources.list.d/StackFlow.list

apt update
apt install llm-whisper llm-vad llm-model-whisper-tiny llm-model-silero-vad
```

### Offline install (no internet on the module)

When the module has no Ethernet/WiFi, the fastest path is: download `.deb` files on the PC, push them via ADB, install locally with `dpkg -i`.

**1. Fetch the package list from the repo** to find filenames and dependencies:

```bash
curl -O https://repo.llm.m5stack.com/m5stack-apt-repo/dists/jammy/ax630c/binary-arm64/Packages
```

**2. Download `.deb` files** (path is in the `Filename:` field of the Packages file):

```bash
curl -fsSL -o llm-whisper_1.5.deb \
  "https://repo.llm.m5stack.com/m5stack-apt-repo/pool/jammy/ax630c/v1.5/llm-whisper_1.5-m5stack1_arm64.deb"
```

**3. Push to the module:**

```bash
MSYS_NO_PATHCONV=1 adb push *.deb /root/pkgs/
```

**4. Inspect a package before installing:**

```bash
adb shell "dpkg-deb -I /root/pkgs/<package>.deb"   # metadata + dependencies
adb shell "dpkg-deb -c /root/pkgs/<package>.deb"   # contained files
```

**5. Install:**

```bash
adb shell "dpkg -i /root/pkgs/<package>.deb"
# on dependency version conflicts:
adb shell "dpkg -i --force-depends /root/pkgs/<package>.deb"
```

**6. Reload / start the service:**

```bash
adb shell "systemctl daemon-reload && systemctl start llm-whisper llm-vad"
adb shell "systemctl is-active llm-whisper llm-vad"
adb shell "journalctl -u llm-whisper -n 20 --no-pager"   # if anything misbehaves
```

### When `--force-depends` is OK

The LLM module shipped with `lib-llm 1.3`, but the repo only carries `lib-llm 1.6` and newer. Model packages (`llm-model-*`) declare `Depends: lib-llm (>= 1.6)` but contain **only data files** (`.axmodel`, `.ort`, `.json`) — no libraries with ABI binding. In that case `--force-depends` is safe.

Service binaries (`llm-whisper`, `llm-vad` in v1.5) only declare `Depends: lib-llm` without a version constraint, so they install cleanly against the older `lib-llm 1.3` too.

**Rule of thumb:** before using `--force-depends`, run `dpkg-deb -c <package>.deb` and check that only data files are inside. For `.so` files or binaries with version constraints, do NOT force — instead upgrade the framework package (`lib-llm`) and all dependent `llm-*` packages together to a consistent version.

### Gotcha: MeloTTS model selection on `lib-llm < 1.6`

In `api_melotts.cpp` of the `M5Module-LLM` library, the resolution `language` → `model` depends on the module framework version:

```cpp
if (version >= 1.6) {
    // language → "melotts-zh-cn" / "melotts-ja-jp" / "melotts-en-default"
} else {
    if (language == "zh_CN") doc["data"]["model"] = "melotts_zh-cn";
    // otherwise: stays config.model (default: "melotts_zh-cn")
}
```

On a module with `lib-llm 1.3`, the default `"melotts_zh-cn"` for any language ≠ `zh_CN` is **not** overridden. Result: TTS sounds Chinese, even though `language` is set correctly and the work_id has the right suffix.

Sketch-side workaround — set `model` explicitly; the directory name under `/opt/m5stack/data/` is the source of truth:

```cpp
m5_module_llm::ApiMelottsSetupConfig_t cfg;
cfg.model = "melotts-en-us";   // matches /opt/m5stack/data/melotts-en-us/
cfg.input = {"tts.utf-8.stream", llm_work_id};
melotts_work_id = module_llm.melotts.setup(cfg, "melotts_setup", "en_US");
```

Naming inconsistency in the repo: the older `melotts_zh-cn` is spelled with an underscore, all newer models (`melotts-en-us`, `melotts-en-default`, `melotts-ja-jp`, …) with a dash. When in doubt, run `ls /opt/m5stack/data/` on the module.

### Gotcha: wake confirmation plays a WAV file

On a wake event, the module plays a **pre-recorded WAV file** — independent of the TTS engine, independent of `enkws` flags in the service configs. The files live at:

```
/opt/m5stack/data/audio/wakeup_en_us.wav
/opt/m5stack/data/audio/wakeup_zh_cn.wav
```

`wakeup_en_us.wav` is by default a spoken "Hi" with a male voice. To **disable** the confirmation or replace it with your own sound, swap the file:

```bash
# back up the original
adb shell "cp /opt/m5stack/data/audio/wakeup_en_us.wav /opt/m5stack/data/audio/wakeup_en_us.wav.bak"

# push your own file — format must match: 16 kHz, mono, 16-bit PCM
adb push my_wakeup.wav /opt/m5stack/data/audio/wakeup_en_us.wav
```

A **silent WAV** (50 ms, same format) is in the project under [`../pkgs/audio/silent_wakeup.wav`](../pkgs/audio/silent_wakeup.wav). Custom sounds must use the same format (16 kHz mono 16-bit PCM), otherwise the audio service refuses or sounds distorted.

The wake confirmation plays on the **LLM module's speaker**. The CoreS3 has its own speaker and can independently play a beep via `M5.Speaker.tone()` — see `playWakeBeep()` in the sketch.

### Gotcha: `llm-melotts` service produces unwanted audio

If you only use the `tts` API (not `melotts`), the parallel `llm-melotts` service still reacts to wake events and plays a default greeting in the voice of the most recently loaded model (e.g. Chinese, if only `melotts_zh-cn` is installed). Cleanest fix: turn the service off.

```bash
adb shell "systemctl stop llm-melotts && systemctl disable llm-melotts"
```

`Restart=always` in the unit file is neutralised by `disable` (it removes the `multi-user.target.wants/` symlink), so the change survives reboots unless someone explicitly `enable`s it again.

### Gotcha: TTS streaming aborts after the first response (v1.3 framework)

When chaining `llm_work_id → tts.utf-8.stream` via `cfg.input = {"tts.utf-8.stream", llm_work_id}`, the subscription on `llm-tts 1.3` is **one-shot**: after the first response's `finish:true`, no audio comes for the second.

Sketch-side workaround: do **not** chain TTS to the LLM stream — instead collect the full LLM response text in a buffer and synthesise it explicitly via `module_llm.tts.inference(tts_work_id, text)` after `finish:true`:

```cpp
// setup
m5_module_llm::ApiTtsSetupConfig_t cfg;
cfg.input = {"tts.utf-8.stream"};   // do NOT chain to llm_work_id
tts_work_id = module_llm.tts.setup(cfg, "tts_setup", "en_US");

// loop
String llm_response_buffer;  // global

if (msg.work_id == llm_work_id && msg.object == "llm.utf-8.stream") {
    llm_response_buffer += delta;
    if (isFinish) {
        module_llm.tts.inference(tts_work_id, llm_response_buffer);
        llm_response_buffer = "";
    }
}
```

This makes multi-turn dialogues reliable.

### Verification: did a service setup actually succeed?

In the Arduino `M5Module-LLM` API, every `setup()` returns a `work_id`:

- **Success**: `<unit>.<id>`, e.g. `whisper.1003`, `vad.1002`
- **Failure** (model missing, service not installed): just the bare unit name, e.g. `whisper`, `vad`

When debugging, log `work_id` and watch for the `.NNNN` suffix.

---

## Available models / language support

State of the M5Stack repo research (April 2026):

| Component | Packages | Languages |
|---|---|---|
| **Whisper** | `llm-model-whisper-tiny`, `-base`, `-small` | multilingual (incl. German) |
| **VAD** | `llm-model-silero-vad` | language-agnostic |
| **Sherpa-ASR** (streaming) | `llm-model-sherpa-ncnn-streaming-zipformer-{20M-en,zh-14M}`, `…-bilingual-zh-en` | en, zh, en+zh |
| **KWS** | `llm-model-sherpa-onnx-kws-zipformer-{gigaspeech,wenetspeech}` | en, zh |
| **MeloTTS** | `llm-model-melotts-{en-us,en-au,en-br,en-india,en-default,es-es,ja-jp,zh-cn}` | **no German** |
| **TTS (legacy)** | `llm-model-single-speaker-{english-fast,fast}` | en, zh |
| **LLM** | `llm-model-qwen2.5-{0.5B,1.5B}`, `llm-model-qwen3-0.6B`, `llm-model-llama3.2-1B`, `llm-model-deepseek-r1-1.5B`, `llm-model-internvl2.5-1B`, `llm-model-smolvlm-256M/500M`, … | multilingual (model-dependent) |

**Consequences for the pet-controller setup (final):**

- ASR in German: ✅ via Whisper-base with `language="de"`
- LLM classification: ✅ Qwen3-0.6B with English prompt + German keyword hints (`lieb=love`)
- TTS on the LLM module: **disabled** (`llm-tts` and `llm-melotts` via `systemctl disable`) — the pet has its own sounds on the CoreS3
- Wake confirmation WAV (`wakeup_en_us.wav`) replaced with silence — the pet plays its own wake sound (`Sound::Wake`) on the CoreS3 speaker

---

## Network / internet on the module

By default the module has **no internet**:

- `eth0` is `NO-CARRIER` (no RJ45 jack on the M140 itself)
- No WiFi module
- ADB is USB-data only, no TCP/IP

Options to bring it online:

1. **Debug board / LLM Mate accessory** with RJ45 (per the M5Stack docs)
2. **USB-RNDIS / USB tethering** from the PC — not configured out of the box on this firmware
3. **adb reverse + HTTP proxy** on the PC, set in `/etc/apt/apt.conf.d/99proxy`

While the module is offline: download `.deb` packages on the PC, `adb push` them, install with `dpkg -i` (see above).

---

## Workflow summary

**To change module software:**

1. Unplug USB-C from the CoreS3, plug into the LLM module
2. Verify with `adb devices` (should show `axera-ax620e`)
3. Push `.deb`s + `dpkg -i` + restart the service
4. Plug USB-C back into the CoreS3

**To change the sketch:**

1. Close the serial monitor if it's open
2. USB-C on the CoreS3 (if not already)
3. `pio run -e cores3 -t upload --upload-port COM5`
4. Optional: reopen the monitor

---

## Current state of the LLM module (April 2026)

The factory image was `lib-llm 1.3` with `qwen2.5-0.5B-prefill-20e` as the only LLM. The pet-controller migration upgraded the stack. Current state on the module:

### systemd services

| Service | Status | Note |
|---|---|---|
| `llm-sys` | active | StackFlow coordinator |
| `llm-audio` | active | microphone capture, speaker |
| `llm-kws` | active | wake-word detection (Sherpa-ONNX-KWS-Zipformer-Gigaspeech) |
| `llm-vad` | active | voice activity detection (Silero-VAD) |
| `llm-whisper` | active | multi-language ASR (Whisper-base, configured for German) |
| `llm-llm` | active | LLM inference (Qwen3-0.6B) |
| `llm-tts` | **disabled** | silent in the pet use case, no TTS output |
| `llm-melotts` | **disabled** | silent in the pet use case, no TTS output |
| `llm-camera`, `llm-vlm`, `llm-yolo`, `llm-skel` | active | unused, could be disabled |

### Installed model packages

| Package | Version | Use |
|---|---|---|
| `lib-llm` | **1.8** | framework library (raised from 1.3) |
| `llm-sys` | **1.6** | coordinator (raised from 1.3, deps: lib-llm ≥ 1.7) |
| `llm-llm` | **1.8** | LLM service (raised from 1.3, deps: lib-llm ≥ 1.7) — speaks Qwen3 |
| `llm-whisper` | 1.5 | Whisper service (deps: lib-llm) |
| `llm-vad` | 1.5 | VAD service (deps: lib-llm) |
| `llm-model-whisper-tiny` | 0.4 | Whisper-Tiny model, multilingual (now unused, base is better) |
| `llm-model-whisper-base` | 0.4 | Whisper-Base model, multilingual (~300 MB, ~2 s latency) |
| `llm-model-silero-vad` | 0.4 | Silero-VAD model |
| `llm-model-melotts-en-us` | 0.6 | English MeloTTS (now unused after TTS disable) |
| `llm-model-qwen2.5-1.5B-Int4-ax630c` | 0.4 | **Doesn't work** — tokenizer files missing in the package (see below) |
| `llm-model-qwen3-0.6B-ax630c` | 0.4 | **Currently used** — has tokenizer files bundled |

Factory models still on the module (e.g. `qwen2.5-0.5B-prefill-20e`, KWS models, Sherpa-ASR) remain untouched.

### Configuration changes

- `/opt/m5stack/data/audio/wakeup_en_us.wav` replaced with a **silent 50 ms WAV** (see [`../pkgs/audio/silent_wakeup.wav`](../pkgs/audio/silent_wakeup.wav)). Backup at `wakeup_en_us.wav.bak`. Defeats the male "Hi" on wake.

---

## What the firmware tells the module at runtime

The packages above only decide what the module is *capable of*. Which model, language and wake word actually get used is configured by the CoreS3 firmware on every boot via `kws.setup` / `vad.setup` / `whisper.setup` / `llm.setup` over UART. Defaults sit in [`../src/voice_pipeline.h`](../src/voice_pipeline.h):

| Firmware field    | Current value         | Meaning |
|---|---|---|
| `wake_word`       | `"MUFFIN"`            | Word the KWS service listens for. Sherpa-KWS supports arbitrary words; just change the string. |
| `whisper_language`| `"de"`                | ISO code Whisper uses for ASR. `"en"` works out of the box because `whisper-base` is multilingual. |
| `whisper_model`   | `"whisper-base"`      | Model identifier. Maps to the `.deb` filename without the version: `llm-model-whisper-base_*.deb` → `whisper-base`. Switching to `"whisper-tiny"` works if the corresponding model is installed. |
| `llm_model`       | `"qwen3-0.6B-ax630c"` | Same naming rule: `llm-model-qwen3-0.6B-ax630c_*.deb` → `qwen3-0.6B-ax630c`. The 1.5B-Int4 model identifier (`qwen2.5-1.5B-Int4-ax630c`) loads on paper but the package is broken — see the gotcha below. |
| `llm_max_tokens`  | `64`                  | Hard cap; raising it doesn't help with Qwen3 thinking-mode (see the dedicated gotcha). |
| `mic_volume`      | `0.7`                 | Capture-side gain, sent to `audio.setup`. |

**The system prompt** (sent to `llm.setup` as `cfg.prompt`) lives in [`../src/main.cpp`](../src/main.cpp) at the top, in the `SYSTEM_PROMPT` const. It frames Qwen3 as a **tag classifier**: input is a German or English sentence, output is 1–3 lowercase comma-separated tags from a closed list of 23 (`eat`, `pet`, `love`, `laugh`, `sleep`, `wake`, `greet`, `sad`, `startle`, `sing`, `dance`, `ball`, `mouse`, `rattle`, `butterfly`, `plush`, `movie`, `game`, `internet`, `social`, `friends`, `radio`, `idle`). The classifier output is mapped to in-firmware actions by `applyVoiceTag()`. The closed-vocabulary framing is what keeps the 0.6B model usable — open-ended generation hallucinates code or invents German on unclear input.

### Worked example: switch Whisper from German to English

Edit [`../src/voice_pipeline.h`](../src/voice_pipeline.h):

```cpp
const char* whisper_language = "en";   // was "de"
```

Reflash the CoreS3 (`pio run -e cores3 -t upload`). Nothing changes on the LLM module side — Whisper-Base is multilingual, the language code is just sent in the next `whisper.setup`.

---

## Gotcha: Qwen2.5-1.5B-Int4 doesn't load (tokenizer missing)

The package `llm-model-qwen2.5-1.5B-Int4-ax630c v0.4` is **incomplete**: it lacks the `tokenizer/` subdirectory.

Comparison:

- `/opt/m5stack/data/qwen3-0.6B-ax630c/tokenizer/` contains `tokenizer.json`, `tokenizer_config.json`, `vocab.json`, `merges.txt`, `config.json` ✅
- `/opt/m5stack/data/qwen2.5-1.5B-Int4-ax630c/` has **no** `tokenizer/` subdirectory ❌

The `mode_qwen2.5-1.5B-Int4-ax630c.json` references via `tokenizer_type:2` an HTTP tokenizer at `localhost:8080`. The tokenizer subprocess (`/opt/m5stack/scripts/tokenizer_qwen2.5-1.5B-Int4-ax630c.py`) tries to load files via `AutoTokenizer.from_pretrained(...)` — either from a local directory or from HuggingFace. The latter fails because the module has no internet. The former doesn't exist.

**Fix options should you want 1.5B to work later:**

1. Download tokenizer files from [HuggingFace Qwen2.5-1.5B-Instruct-GPTQ-Int4](https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GPTQ-Int4) locally, push to `/opt/m5stack/data/qwen2.5-1.5B-Int4-ax630c/tokenizer/`
2. Adjust the tokenizer script, point `--model_id` at the local path
3. Restart the service

---

## Gotcha: Qwen3 thinking mode

Qwen3 models have a **chain-of-thought mode** active by default. On every inference call the model first generates a `<think>...</think>` block (reasoning), then the actual answer.

Without adjustment, a tag-classification output looks like this:

```
<think>
The user said "Hier hast du Futter" which means feeding the pet...
</think>
[EAT]
```

**Consequences:**

- With small `max_token_len` (≤ 60), generation aborts **mid-thinking** → empty answer
- Token usage (and thus latency) is significantly higher

**Workarounds in the sketch:**

- `cfg.max_token_len = 64` — enough headroom for an empty `<think></think>` (with `/no_think`) plus a comma-separated list of up to three tags
- User input is suffixed with `" /no_think"` — a Qwen3 soft switch that suppresses thinking (doesn't always hit 100 %)
- The tag parser strips `<think>...</think>` blocks before keyword matching; if `<think>` appears without a closing tag (truncated reasoning), it returns IDLE

**Heavier solution** (not implemented but documented): edit the tokenizer script to pass `enable_thinking=False` to `apply_chat_template`:

```python
# in /opt/m5stack/scripts/tokenizer_qwen3-0.6B-ax630c.py, encode():
text = self.tokenizer.apply_chat_template(
    messages,
    tokenize=False,
    add_generation_prompt=True,
    enable_thinking=False,   # <-- new
)
```

To apply: plug USB into the module, edit the file, kill the tokenizer subprocess (it gets respawned by the `llm-llm` service).

---

## Module changes — chronological log

What was done to this module beyond the factory image — in the order of actions, so a later reset / reflash can replicate the steps.

| # | Action | Command / path |
|---|---|---|
| 1 | Installed ADB on the PC | `winget install Google.PlatformTools` |
| 2 | Installed Whisper and VAD packages on the module (factory had neither) | `dpkg -i llm-vad_1.5.deb llm-whisper_1.5.deb`, then `dpkg -i --force-depends llm-model-silero-vad_0.4.deb llm-model-whisper-tiny_0.4.deb` |
| 3 | Disabled `llm-melotts` (was producing the unwanted Chinese wake greeting) | `systemctl stop llm-melotts && systemctl disable llm-melotts` |
| 4 | Replaced `wakeup_en_us.wav` with the silent WAV | `adb push silent_wakeup.wav /opt/m5stack/data/audio/wakeup_en_us.wav` (original kept as `.bak`) |
| 5 | Installed the English MeloTTS model (for an English voice during the transitional period) | `dpkg -i --force-depends llm-model-melotts-en-us_0.6.deb` (now unused) |
| 6 | Installed the Whisper-Base model (better than tiny) | `dpkg -i --force-depends llm-model-whisper-base_0.4.deb` |
| 7 | Disabled `llm-tts` (the pet doesn't need TTS) | `systemctl stop llm-tts && systemctl disable llm-tts` |
| 8 | Raised the framework stack (for Qwen3 support): `lib-llm 1.3 → 1.8`, `llm-sys 1.3 → 1.6`, `llm-llm 1.3 → 1.8` | `dpkg -i lib-llm_1.8.deb llm-sys_1.6.deb llm-llm_1.8.deb` |
| 9 | Installed the Qwen2.5-1.5B-Int4 model (doesn't work though — see gotcha above) | `dpkg -i --force-depends llm-model-qwen2.5-1.5B-Int4-ax630c_0.4.deb` |
| 10 | Installed the Qwen3-0.6B model | `dpkg -i --force-depends llm-model-qwen3-0.6B-ax630c_0.4.deb` |
| 11 | Re-disabled TTS services after the lib-llm post-install (postinst had re-enabled them) | `systemctl stop llm-tts llm-melotts && systemctl disable llm-tts llm-melotts` |

All `.deb` files live locally under [`../pkgs/`](../pkgs/) in the project, sorted by function:

- [`../pkgs/framework/`](../pkgs/framework/) — `lib-llm`, `llm-sys`, `llm-llm` (core stack)
- [`../pkgs/services/`](../pkgs/services/) — `llm-vad`, `llm-whisper` (service binaries)
- [`../pkgs/models/`](../pkgs/models/) — all `llm-model-*` (model data)
- [`../pkgs/audio/`](../pkgs/audio/) — `silent_wakeup.wav` (the only tracked file; all `.deb`s are gitignored)

### Full reproduction (worst case: module wiped / re-flashed via M5Burner)

Run in this order, USB plugged into the LLM module:

```bash
# Path to the `adb` executable. If it's already on your PATH (`adb` works
# in your shell), leave this as `adb`. Otherwise point it at your install:
#   Windows:  winget install Google.PlatformTools  (then `adb` is on PATH after a new shell)
#   macOS:    brew install android-platform-tools
#   Linux:    apt install adb  (or distro equivalent)
ADB=adb

# 1) Upload all pkgs — they end up flat under /root/pkgs/, dpkg works
#    with the bare filenames afterwards.
cd pkgs
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

# 2) Framework upgrade
MSYS_NO_PATHCONV=1 "$ADB" shell "cd /root/pkgs && \
    dpkg -i lib-llm_1.8-m5stack1_arm64.deb && \
    dpkg -i llm-sys_1.6-m5stack1_arm64.deb && \
    dpkg -i llm-llm_1.8-m5stack1_arm64.deb && \
    systemctl daemon-reload && \
    systemctl restart llm-sys llm-llm"

# 3) Whisper + VAD
MSYS_NO_PATHCONV=1 "$ADB" shell "cd /root/pkgs && \
    dpkg -i llm-vad_1.5.deb && \
    dpkg -i llm-whisper_1.5.deb && \
    dpkg -i --force-depends llm-model-silero-vad_0.4.deb && \
    dpkg -i --force-depends llm-model-whisper-base_0.4.deb && \
    systemctl daemon-reload && \
    systemctl start llm-vad llm-whisper"

# 4) Qwen3
MSYS_NO_PATHCONV=1 "$ADB" shell "dpkg -i --force-depends /root/pkgs/llm-model-qwen3-0.6B-ax630c_0.4.deb"

# 5) Permanently disable TTS services
MSYS_NO_PATHCONV=1 "$ADB" shell "systemctl stop llm-tts llm-melotts; systemctl disable llm-tts llm-melotts"

# 6) Silence the wake WAV
MSYS_NO_PATHCONV=1 "$ADB" shell "cp /opt/m5stack/data/audio/wakeup_en_us.wav /opt/m5stack/data/audio/wakeup_en_us.wav.bak"
MSYS_NO_PATHCONV=1 "$ADB" push audio/silent_wakeup.wav /opt/m5stack/data/audio/wakeup_en_us.wav

# 7) Verify
MSYS_NO_PATHCONV=1 "$ADB" shell "systemctl is-active llm-sys llm-llm llm-audio llm-kws llm-vad llm-whisper; \
    dpkg -l | grep -E 'lib-llm|llm-llm|llm-sys|llm-vad|llm-whisper|qwen3' | awk '{print \$2, \$3}'"
```

---

## Authors

- **Justus** and **Marcel** — see also the credits screen in the menu.
