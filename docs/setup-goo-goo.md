# Setup: Goo-Goo (M5Stack Core2)

**Difficulty: 🟢 Easiest.** Plug in USB, flash, run. No external module, no button gymnastics. About an hour from unbox to running pet.

## What you need

- **M5Stack Core2** ([store link](https://shop.m5stack.com/products/m5stack-core2-esp32-iot-development-kit-v1-1)) — built-in 500 mAh battery, no extras to buy.
- A **USB-C cable** (the Core2 has a USB-C port on the bottom).
- A computer with [PlatformIO](https://platformio.org/) (`pip install platformio` if you don't have it).

## Flashing

The Core2's CH9102 USB bridge appears on your PC as a regular COM/serial port:

```bash
pio device list
```

Look for **VID:PID `1A86:55D4`** ("USB-Enhanced-SERIAL CH9102"). On Windows it's typically `COM3..7`; on macOS `/dev/cu.wchusbserial...`; on Linux `/dev/ttyACM0` or `/dev/ttyUSB0`.

Then:

```bash
pio run -e core2 -t upload --upload-port COM<N>
```

The build downloads dependencies on first run and takes ~1–2 minutes. The device hard-resets via RTS pin and boots into the firmware.

## Watching the serial monitor

```bash
pio device monitor -p COM<N> -b 115200
```

Useful for first-boot diagnostics (WiFi join, ESP-NOW init, etc.). Exit with `Ctrl+C`.

## Buttons

The Core2 has three physical buttons under the display: **BtnA** (Greet), **BtnB** (Feed), **BtnC** (Sleep). The bottom-edge hint glyphs label them. Touch on the rest of the screen also works.

## Branding

- Splash screen: "Goo-Goo"
- WiFi setup AP: `goo-goo-setup`
- mDNS: `goo-goo.local`

## Common gotchas

- **PWR button** (small red button on the left side): short press = sleep / wake; **≥6 s** = hard power-off via the AXP192. The pet's bedtime sequence handles short presses gracefully — don't hold > 6 s while testing.
- **COM port held by serial monitor**: close `pio device monitor` before re-flashing — `Could not open COM<N>` errors mean the monitor still has the port.

## Partition table

[`../partitions_core2_16MB.csv`](../partitions_core2_16MB.csv) — 16 MB flash, includes the LittleFS section the firmware uses for selfies (none on Core2 — no camera) and persisted state.

## What the Core2 doesn't have (vs. Muffin / Visu)

- No wake word / voice control (no LLM module).
- No camera (so no proximity wake-up, no selfies, no face-detection animations).
- The corresponding help pages (Voice / Camera) are skipped on Core2 in the help UI; their i18n strings remain compiled in.

The shared pet logic — moods, mini-games, foraging, ESP-NOW friends, weather, moon, scenes, sleep regen, gift bar — works exactly as on the bigger pets.

## Next steps

- Open the [README's Help-it-grow section](../README.md#help-it-grow) for low-effort ways to support the project.
- Want a friend for your Goo-Goo? Add a [Pip](setup-pip.md) accessory.
