# Setup: Visu (M5Stack CoreS3)

**Difficulty: 🟢 Easy.** Same hardware as Muffin minus the Module-LLM expansion. Plug in, flash, run. About an hour to a running pet.

## What you need

- **M5Stack CoreS3** ([store link](https://shop.m5stack.com/products/m5stack-cores3-esp32s3-iot-development-kit))
- **M5GO Battery Bottom3** ([store link](https://shop.m5stack.com/products/m5go-battery-bottom3-for-cores3-only)) — 500 mAh, the official CoreS3 accessory.
- A **USB-C cable**.
- A computer with [PlatformIO](https://platformio.org/) (`pip install platformio` if missing).

## Flashing

The CoreS3 has a native ESP32-S3 USB-CDC port (no bridge chip):

```bash
pio device list
```

Look for **VID:PID `303A:1001`** (Espressif USB JTAG/serial debug unit). On Windows typically `COM5..7`.

```bash
pio run -e visu -t upload --upload-port COM<N>
```

Takes ~1–2 minutes. The device resets and boots into the firmware.

## Serial monitor

```bash
pio device monitor -p COM<N> -b 115200
```

## What's different vs. Muffin?

Visu is the same hardware as Muffin minus the Module-LLM expansion. So:

- ✅ Touch screen
- ✅ Front camera (proximity wake-up + selfies + 5-slot LittleFS gallery)
- ✅ Web radio
- ❌ Voice control (no LLM module)

The voice pipeline is excluded from the `visu` env via `build_src_filter`, so the `cores3` build's voice features don't ship in the Visu binary — no extra dependencies pulled in.

## Branding

- Splash: "Visu"
- WiFi setup AP: `visu-setup`
- mDNS: `visu.local`

## Common gotchas

- **COM port conflict**: while the serial monitor is open, `COM<N>` is held exclusively. Close it before flashing.
- **Native USB-CDC quirks**: auto-reset via DTR/RTS works fine on the CoreS3. (It does **not** work on the StickC PLUS2 S3 revision — see [setup-pip.md](setup-pip.md).)

## Want voice too?

Add the optional Module-LLM expansion + Battery Module 13.2 and switch to the `cores3` env — see [setup-muffin.md](setup-muffin.md). Same CoreS3 firmware path, plus an extra board, plus the Module-LLM software setup.
