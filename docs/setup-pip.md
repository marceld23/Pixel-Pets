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

### Manual download-mode procedure

(Verified on the actual hardware — earlier shorter variants don't always take.)

1. **Disconnect USB** completely.
2. **Hold BtnA** (the front button under the M5 logo) and keep holding.
3. While still holding BtnA, **press and hold the side reset button** for ~2 s.
4. Release the reset button, keep holding BtnA for ~1 more second, then release BtnA.

The display stays black — the chip is now in download mode. Plug USB back in.

```bash
pio device list
```

The COM port has changed: look for **`303A:1001`** (USB-CDC) now. Flash with that port:

```bash
pio run -e pip-s3 -t upload --upload-port COM<N>
```

After a successful flash, the device hard-resets via RTS and re-enumerates back as `303A:8120` for normal use.

### Recovery: stuck in ROM bootloader after flash

A frequent failure mode on the S3 stick: the flash succeeds, esptool prints `Hard resetting via RTS pin...`, but the chip stays in ROM bootloader (display black, slow green LED blink, COM port still `303A:1001`). The auto-reset is genuinely unreliable on native USB-CDC because the RTS pin isn't physically wired the way esptool assumes.

Recovery, in order of likelihood:

1. **Side reset button.** PLUS2 has a tiny hardware reset on the right edge (above BtnB). One short press → clean boot from flash.
2. **USB unplug → 5 s wait → plug back in.** No buttons held. The 5-second wait lets residual capacitance drain so GPIO0 strapping is read fresh on power-up.
3. **Mechanically un-stick BtnA.** GPIO0 is the boot strap pin and on the PLUS2 it's wired to BtnA. After repeated download-mode entries the BtnA microswitch can stick partially-pressed and read low at boot — chip enters download mode every time. Click BtnA gently 20–30× rapidly to dislodge, then unplug + replug USB.
4. **Drain the battery.** If everything else fails, leave unplugged for 1–2 hours. The 200 mAh battery discharges enough to fully reset the SoC. Slow but bulletproof.

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
