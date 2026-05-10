# Setup: Pip (M5StickC PLUS2)

**Difficulty: 🟡 Medium.** The S3 revision of the PLUS2 has a quirky download-mode procedure that the original ESP32-PICO version doesn't need. Once that's behind you, Pip is fast to flash and small to debug.

Pip is **not** a stand-alone pet — it's a pocket accessory that pairs with one of the bigger pets (Muffin / Visu / Goo-Goo) over ESP-NOW. Set up a home pet first if you haven't.

## What you need

- **M5StickC PLUS2** ([store link](https://shop.m5stack.com/products/m5stickc-plus2-esp32-mini-iot-development-kit)) — built-in 200 mAh battery.
- A **USB-C cable**.
- A computer with [PlatformIO](https://platformio.org/) (`pip install platformio` if missing).

There are two PLUS2 hardware revisions in circulation; the env you flash with depends on which you have.

## Identify your PLUS2 revision

Plug the device in and:

```bash
pio device list
```

| VID:PID | Revision | Use env |
|---|---|---|
| `1A86:55D4` (CH9102) | Original ESP32-PICO PLUS2 | `pip` |
| `303A:8120` (USB JTAG/serial debug unit) | S3 revision (newer) | `pip-s3` |
| `303A:1001` (USB-CDC) | S3 revision currently in **download mode** | `pip-s3` |

If you only see `303A:1001`, the device is already in download mode — no need for the procedure below.

## ESP32-PICO PLUS2 (`pip` env) — easy path

Auto-reset via DTR/RTS works normally. Just flash:

```bash
pio run -e pip -t upload --upload-port COM<N>
```

That's it.

## S3 revision (`pip-s3` env) — manual download-mode

The auto-reset that PlatformIO normally uses to put the chip into download mode does **not** work reliably on the S3 with native USB-CDC. The build will succeed but the upload step may fail with:

```
A serial exception error occurred: Write timeout
```

You need to put the chip into download mode manually before flashing. The procedure differs between the two S3 hardware variants in circulation; pick the one that matches your device.

> **Note on hardware variants.** Both the renamed **M5StickS3** (SKU K150, current product) and the older **M5StickC PLUS2 S3 revision** enumerate as VID `303A:1001`/`303A:8120`. Check whether your stick has a separate hardware reset button next to BtnB:
>
> - **No separate reset button → M5StickS3 (K150).** Only one button on the side (Power/PMIC). Use the bridge procedure below.
> - **Has a tiny reset button on the side → original PLUS2 S3 revision.** Use the BtnA + reset procedure further down.
>
> The original ESP32-PICO PLUS2 (`pip` env) doesn't need any of this — it has working DTR/RTS auto-reset.

### M5StickS3 (K150) — Hat2-Bus G0 bridge procedure

On this variant **BtnA is GPIO11**, not GPIO0, so holding BtnA does nothing for download-mode entry. There's also no separate reset button. The reliable way in is to short GPIO0 to GND on the Hat2-Bus during a cold boot.

The 16-pin Hat2-Bus on the bottom has these labels (silkscreen on the back):

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | GND | 2 | EXT_5V |
| 3 | G5 | **4** | **G0 (BOOT)** |
| 5 | G6 | 6 | G1 |
| 7 | G7 | 8 | G8 |
| 9 | G43 (UART0 TX) | 10 | BAT |
| 11 | G44 (UART0 RX) | 12 | 3V3_L2 |
| 13 | G2 | 14 | 5V_IN |
| 15 | G3 | 16 | — |

Procedure:

1. **Power off the stick.** Two quick presses of the Power button → off (the PM1 PMIC interprets a double-tap as power-off; a single short press is power-on, a long press depends on PMIC firmware revision and is unreliable as a force-off). LED goes out, display goes black.
2. **Bridge G0 (pin 4) to GND (pin 1)** on the Hat2-Bus header. A jumper wire, tweezers, or a paperclip works.
3. **Press Power once** → PM1 powers up the ESP32-S3, GPIO0 reads LOW at boot strap, ROM bootloader starts.
4. **Confirm: the LED blinks slow green at ~1 Hz.** That's the ROM bootloader's "I'm in download mode" indicator. If the LED is off or solid, the strap wasn't read low — re-seat the bridge and retry from step 1.
5. **Plug USB.** The chip enumerates as `303A:1001` (USB JTAG/serial debug unit) with serial number `00:00:00:00:00:00`.
6. **Remove the bridge.** Once the LED is blinking, GPIO0 has been sampled and we don't need it low anymore — leaving the bridge dangling on the header risks intermittent shorts to neighbouring pins during the flash.
7. Flash:

   ```bash
   pio run -e pip-s3 -t upload --upload-port COM<N>
   ```

8. After flash, esptool resets the chip via RTS. On this variant RTS isn't wired through, so the reset doesn't always take — see "Recovery" below.

### Original PLUS2 S3 revision (with side reset button) — BtnA + reset procedure

If your stick has a tiny hardware reset button on the right edge (above BtnB), it's the older PLUS2 S3 revision, where BtnA is wired in parallel to GPIO0 (the strap pin):

1. **Disconnect USB** completely.
2. **Hold BtnA** (the front button under the M5 logo) and keep holding.
3. **Press and hold the side reset button** for ~2 s.
4. Release reset, keep holding BtnA for ~1 more second, then release BtnA.

Display stays black, LED blinks 1 Hz green = download mode. Plug USB back in, flash with `pio run -e pip-s3 -t upload`.

### Recovery: stuck in ROM bootloader after flash

A frequent failure mode on either S3 variant: the flash succeeds, esptool prints `Hard resetting via RTS pin...`, but the chip stays in ROM bootloader (display black, slow green LED blink, COM port still `303A:1001`). The RTS line isn't physically wired the way esptool assumes.

Recovery, in order:

1. **PLUS2 S3 (with reset button):** one short press of the side reset button → clean boot from flash.
2. **M5StickS3 (K150, no reset button):** two-press Power-off → wait 2 s → single-press Power-on. Chip cold-boots from flash without the GPIO0 bridge.
3. **USB unplug → 5 s wait → plug back in.** Works on both variants. Lets residual capacitance drain so GPIO0 reads fresh-high on power-up.
4. **Mechanically un-stick BtnA (PLUS2 only).** On the PLUS2 S3, BtnA shares GPIO0 with the strap pin. After repeated download-mode entries the microswitch can stick partially-pressed and read low at boot — chip re-enters download mode every reset. Click BtnA gently 20–30× rapidly to dislodge, then unplug + replug.
5. **Drain the battery.** Last resort. Leave unplugged 1–2 hours; the 200 mAh battery discharges enough to fully reset the SoC.

### When USB enumeration doesn't work at all

If your S3 stick is in download mode (LED blinking 1 Hz green confirmed) but Windows/macOS/Linux never sees it as `303A:1001`, and:

- Different USB cable doesn't help
- Different USB port doesn't help
- Different computer doesn't help

…then the USB-C connector's **D+/D- contacts are mechanically loose** (power pins still work, hence LED + charging) or the chip's USB-PHY is damaged. Software can't fix this. Fall back to UART flashing through the Hat2-Bus.

#### UART recovery flash (M5StickS3 K150)

You need a USB-TTL serial adapter (FTDI FT232RL, CP2102, CH340 — any will do). Wire it to the Hat2-Bus:

| USB-TTL adapter | Stick S3 Hat2-Bus | Notes |
|---|---|---|
| TX | Pin 11 (G44 / U0RXD) | adapter→chip |
| RX | Pin 9 (G43 / U0TXD) | chip→adapter |
| GND | Pin 1 (GND) | required |
| 3V3 / 5V | **leave unconnected** | the Stick has its own battery |

Do **not** connect the adapter's VCC line to the Stick — back-feeding power into a battery-powered device is a good way to brick the PMIC.

Then enter download mode the usual way (G0/GND bridge on pins 4/1 + Power-button cold-boot). LED should blink 1 Hz green. The adapter's COM port is what we flash through:

```bash
~/.platformio/penv/Scripts/python.exe \
  ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 \
  --port COM<adapter> \
  --baud 115200 \
  --before no_reset \
  --after hard_reset \
  write_flash --flash_size keep \
  0x0      .pio/build/pip-s3/bootloader.bin \
  0x8000   .pio/build/pip-s3/partitions.bin \
  0x10000  .pio/build/pip-s3/firmware.bin
```

`--before no_reset` is important: simple USB-TTL adapters don't expose DTR/RTS, so esptool can't auto-reset, and we already entered download mode manually. After flash, remove the bridge and double-press Power to cycle the chip into the new firmware.

### Don't enable `-DARDUINO_USB_CDC_ON_BOOT=1` on `[env:pip-s3]`

This Arduino flag remaps `Serial` to native USB-CDC so `pio device monitor` can read `Serial.printf` output. Tempting, but **adding it to `[env:pip-s3]` causes the firmware to fail to leave the ROM bootloader after flash** — the auto-reset path gets even more confused. If you need on-Pip diagnostics, use ESP-IDF logging (`ESP_LOGE` / `esp_log_*`) which writes to USB-Serial/JTAG natively without the flag, or wire the device's UART pins to an external USB-TTL adapter.

## Pairing with a home pet

Once Pip is flashed and a home pet (Muffin / Visu / Goo-Goo) is also flashed and running, enable the listener on the home pet:

**Settings → Pip mode → On**

Off by default — the always-on ESP-NOW listener costs ~20 % battery on the home pet. With it on, BtnA on Pip cycles through five pages: empty "pick one", Apple / Carrot / Bone / Tricks. On a treat page, a wrist-flick sends the gift to the home pet via ESP-NOW; within ~200 ms the home pet eats it (`Face::Eating` + Eat sound + happiness/fullness boost). Pip itself stays radio-off until each shake — its 200 mAh battery is essentially unaffected.

## Branding

- Splash: "Pip"
- No mDNS / WiFi setup AP — Pip doesn't run a WiFi stack of its own (ESP-NOW only, no router).

## Partition table

[`../partitions_pip_8MB.csv`](../partitions_pip_8MB.csv) — 8 MB flash, smaller than the bigger pets.
