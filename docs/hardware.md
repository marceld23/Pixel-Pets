# Hardware setup

Pixel Pets runs on five build targets. Setup difficulty varies a lot — pick the per-pet guide that matches the hardware you have:

| Pet | Hardware | Difficulty | Setup guide |
|---|---|---|---|
| **Goo-Goo** | M5Stack Core2 | 🟢 Easiest — plug, flash, run | [`setup-goo-goo.md`](setup-goo-goo.md) |
| **Visu** | M5Stack CoreS3 | 🟢 Easy — same idea on CoreS3 | [`setup-visu.md`](setup-visu.md) |
| **Pip** | M5StickC PLUS2 | 🟡 Medium — S3 revision needs a manual download-mode procedure | [`setup-pip.md`](setup-pip.md) |
| **Muffin** | M5Stack CoreS3 + Module-LLM | 🔴 Advanced — extra hardware + ADB / Linux setup on the LLM module | [`setup-muffin.md`](setup-muffin.md) |

The [README's Fastest start](../README.md#fastest-start) has the same overview with shop links. If you're new, start with **Goo-Goo** (if you have a Core2) or **Visu** (if you have a CoreS3) — both are ~1 hour from unbox to running pet. Muffin adds another evening for the Module-LLM setup.

## Build targets — capability matrix

| Target | Board | SoC | LLM | Camera | Hard buttons | Display | Audio | Branding |
|---|---|---|---|---|---|---|---|---|
| `cores3` | M5Stack CoreS3 + Module-LLM | ESP32-S3 | ✅ | ✅ | ❌ Touch strip | 320×240 | WAV | Muffin |
| `visu`   | M5Stack CoreS3 without module | ESP32-S3 | ❌ | ✅ | ❌ Touch strip | 320×240 | WAV | Visu |
| `core2`  | M5Stack Core2 | ESP32 | ❌ | ❌ | ✅ BtnA/B/C | 320×240 | WAV | Goo-Goo |
| `pip`    | M5StickC PLUS2 (ESP32 PICO) | ESP32 | ❌ | ❌ | ✅ BtnA/B | 135×240 | Buzzer | Pip |
| `pip-s3` | M5StickC PLUS2 (S3 revision) | ESP32-S3 | ❌ | ❌ | ✅ BtnA/B | 135×240 | Buzzer | Pip |

Default env: `cores3`. Build everything in the matrix:

```bash
pio run -e cores3 -e visu -e core2 -e pip -e pip-s3
```

## Capability flags

Per-target capabilities are derived in [`../src/target_caps.h`](../src/target_caps.h) from the `TARGET_*` build flags:

- `TARGET_HAS_LLM` → voice pipeline (KWS / VAD / Whisper / Qwen3)
- `TARGET_HAS_CAMERA` → front camera + face detection
- `TARGET_HAS_HARD_BUTTONS` → physical BtnA/B/C
- `TARGET_HAS_TOUCH` → touchscreen
- `TARGET_HAS_WAV_AUDIO` → WAV speaker (otherwise PWM buzzer)
- `TARGET_HAS_WIFI` → active WiFi (NTP, ip-api, web radio, ESP-NOW friends, captive portal)
- `TARGET_DISPLAY_W` / `TARGET_DISPLAY_H` → display dimensions
- `TARGET_NAME` / `TARGET_AP_NAME` / `TARGET_MDNS_NAME` → branding

`#if TARGET_HAS_LLM` and similar gates wrap target-specific code so the same source tree builds for all five envs.

## Authors

**Justus** and **Marcel** — see also the in-app credits screen, and [`../CREDITS.md`](../CREDITS.md).
