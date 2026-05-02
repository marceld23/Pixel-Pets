# Project tools

Small Python helpers that operate on data the device emits over Serial.

The Python environment is managed with [`uv`](https://docs.astral.sh/uv/) (fast, no global installs, lockfile-based). All commands below assume you've installed `uv` once on your machine.

## One-time setup

```bash
# Install uv (Windows: choose the installer that fits)
winget install astral-sh.uv          # Windows
# or
brew install uv                       # macOS
# or
curl -LsSf https://astral.sh/uv/install.sh | sh   # Linux/macOS shell

# From the repo root, sync the tool dependencies
cd tools
uv sync
```

`uv sync` creates `tools/.venv` and installs everything from `pyproject.toml` (currently just Pillow). No global pollution, no `pip install` against system Python.

## `extract_screenshots.py`

Decodes screenshot dumps emitted by the firmware (built with one of the `*-shots` envs) into PNG files for the README and docs.

### How to capture

1. **Flash a screenshot env.** Each base target has a `*-shots` variant that adds `-DSCREENSHOT_MODE=1`:

   ```bash
   pio run -e cores3-shots -t upload --upload-port COM5     # CoreS3 / Muffin
   pio run -e visu-shots   -t upload --upload-port COM5     # Visu
   pio run -e core2-shots  -t upload --upload-port COMxx    # Core2 / Goo-Goo
   ```

   The screenshot envs are deliberately **not** part of the CI matrix and not used for production binaries — they exist purely to grab images for the docs.

2. **Open the serial monitor and pipe to a log file:**

   ```bash
   pio device monitor -p COM5 -b 115200 | tee capture.log
   ```

3. **On the device:** navigate to whichever screen you want to capture, then **press the PWR button for ≥ 2 seconds**. The firmware dumps the canvas as base64 RGB565 over Serial. Repeat for as many screens as you need.

4. **Decode** (in another terminal, or after stopping the monitor):

   ```bash
   uv run --project tools tools/extract_screenshots.py capture.log screenshots/
   ```

   Or live-stream into the script:

   ```bash
   pio device monitor -p COM5 -b 115200 | uv run --project tools tools/extract_screenshots.py - screenshots/
   ```

5. PNGs land in `screenshots/<screen-name>_<seq>.png` (e.g. `pet_01.png`, `media-select_01.png`, `gallery_02.png`, …).

### Screen-name picker

The firmware names each dump after whichever modal/screen is currently active (`pet`, `settings`, `media-select`, `camera`, `gallery`, `friends`, `sport-select`, `sport-workout`, `activity`, `foraging`, `cleaning`, `timer`, `travel-select`, …) — see `currentScreenName()` in `src/main.cpp`. Just navigate to the screen you want before holding PWR.

### After capturing

When you're done, re-flash with the production env (`cores3` / `visu` / `core2`) to remove the screenshot helper from the on-device firmware.
