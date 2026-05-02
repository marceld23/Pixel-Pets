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

## Screenshot workflow

Three Python scripts collaborate with the `*-shots` firmware envs to grab
canvas dumps over Serial and turn them into PNGs:

- [`shot.py`](shot.py) — opens the COM port (without auto-resetting the
  ESP32), sends one screenshot command, captures the response, appends to
  `capture.log`, exits. One-shot, scriptable.
- [`listen.py`](listen.py) — long-running serial listener; appends every
  byte to `capture.log`. Use it together with `shot all` for free-form
  manual navigation.
- [`extract_screenshots.py`](extract_screenshots.py) — decodes a
  `capture.log` into one PNG per dump.

### 1. Flash a screenshot env

Each base target has a `*-shots` variant that adds `-DSCREENSHOT_MODE=1`:

```bash
pio run -e cores3-shots -t upload --upload-port COM5     # CoreS3 / Muffin
pio run -e visu-shots   -t upload --upload-port COM5     # Visu
pio run -e core2-shots  -t upload --upload-port COMxx    # Core2 / Goo-Goo
```

The screenshot envs are deliberately **not** part of the CI matrix and
not used for production binaries — they exist purely to grab images for
the docs.

### 2. Take the shots

The firmware listens for these line-based commands on the serial port
(case-insensitive, terminated by `\n`):

| Command         | Effect |
|---|---|
| `shot`          | Dump the current canvas once. |
| `shot all`      | Auto-dump on every screen-name OR language change. |
| `shot stop`     | Disable auto-dump (also aborts an animals cycle). |
| `shot animals`  | Cycle Bear → Cat → Dog while forcing eyes-open Idle frames; dumps each. |
| `shot lang de`  | Switch UI language to German at runtime. |
| `shot lang en`  | Switch UI language to English at runtime. |
| `shot help`     | Print the command list to Serial. |

Filenames are auto-built from the active screen + language, so DE/EN
captures land in distinct files (`pet_de_01.png`, `pet_en_01.png`,
`pet_bear_de_01.png`, …).

#### Single screen

```bash
uv run --project tools tools/shot.py --port COM3
```

(Defaults to `$PIXELPETS_PORT` or `COM3`. Pass `--cmd "shot animals"` to
trigger the cycle in one call — `shot.py` waits long enough for all
three dumps.)

#### Manual walk through many screens

```bash
# Terminal 1 — run forever, capture every dump
uv run --project tools tools/listen.py --port COM3

# Terminal 2 — turn on auto-dump
uv run --project tools tools/shot.py --port COM3 --cmd "shot all"
# … now navigate at the device; every screen / language change dumps …

# When done, stop the listener (Ctrl-C) and disable auto-dump
uv run --project tools tools/shot.py --port COM3 --cmd "shot stop"
```

The animals cycle in one call, both languages:

```bash
uv run --project tools tools/shot.py --port COM3 --cmd "shot lang de" --timeout 5
uv run --project tools tools/shot.py --port COM3 --cmd "shot animals"
uv run --project tools tools/shot.py --port COM3 --cmd "shot lang en" --timeout 5
uv run --project tools tools/shot.py --port COM3 --cmd "shot animals"
```

(Legacy: holding the PWR button ≥ 2 s also triggers a dump on CoreS3.
The Core2's PEK button only fires click events, so the serial commands
are the reliable path there.)

### 3. Decode

```bash
uv run --project tools tools/extract_screenshots.py capture.log screenshots/
```

PNGs land in `screenshots/<name>_<seq>.png`. The dumper labels each
header with `fmt=rgb565be` (M5GFX sprite buffers are big-endian on
ESP32); the decoder honours both `rgb565be` and the legacy `rgb565le`.

### Screen-name picker

The firmware names each dump after whichever modal/screen is currently
active (`pet`, `settings`, `media-select`, `camera`, `gallery`,
`friends`, `sport-select`, `sport-workout`, `activity`, `foraging`,
`cleaning`, `timer`, `travel-select`, …) — see `currentScreenName()` in
`src/main.cpp`. Just navigate to the screen you want before sending
`shot`.

### After capturing

When you're done, re-flash with the production env (`cores3` / `visu` /
`core2`) to remove the screenshot helper from the on-device firmware.
