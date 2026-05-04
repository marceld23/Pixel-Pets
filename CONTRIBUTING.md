# Contributing to Pixel Pets

Thanks for being interested. Pixel Pets is a small father-and-son hobby project — we're not a company, there's no contributor agreement, and no Slack to join. Contributing is just GitHub: open an issue, send a PR, share your build.

The README's [Help it grow](README.md#help-it-grow) section covers the high-level *what's the most useful thing I can do?* question. This file covers the practical bits.

## Setting up the dev environment

The firmware is built with [PlatformIO](https://platformio.org/). Quickest path:

```bash
git clone https://github.com/marceld23/Pixel-Pets.git
cd Pixel-Pets
pip install platformio        # if you don't already have it
pio run -e core2              # build for Goo-Goo
```

See the [Fastest start](README.md#fastest-start) table for which env matches which board. Useful commands:

- `pio run -e <env> -t upload` — flash a real device
- `pio test -e native` — run the pure-logic unit tests (no hardware needed)
- `pio run` — build every target in the matrix

The web landing page lives under [`site/`](site/) and is plain HTML / CSS / JS, no build step. Edit and reload.

## What's a good first contribution

In rough order from "good first" to "larger":

- **A new sound.** Drop a WAV in `assets/sounds/<name>.wav` and add the corresponding header in `src/sounds/`. See [`docs/sound_assets.md`](docs/sound_assets.md) for the spec.
- **A new face animation, mood or pet-mini-renderer.**
- **A new mini-game or scene.**
- **A new Pip interaction** (the pocket accessory) — extend the message types in [`src/pip_link.h`](src/pip_link.h).
- **Improvements to the German↔English language strings** in [`src/i18n.cpp`](src/i18n.cpp).
- **Documentation, screenshots, real-hardware photos.**

## What's probably out of scope

- Fork-style rewrites of the architecture. The codebase is small and intentional; please open an issue first so we can discuss before you spend a weekend on it.
- Cloud / account features. The pet is local-first by design — see the [For parents](README.md#for-parents) section.
- New build targets for boards we don't own. We can't usefully review or test those.

## Pull requests

Keep them focused — one feature or fix per PR. Touch as few files as needed. The PR template will ask for:

- What changed.
- Why.
- Which target(s) you tested on (real hardware vs. native vs. build-only).

If your change is UI-affecting, a before/after screenshot or short video is genuinely helpful. The [screenshot tooling under `tools/`](tools/README.md) exists exactly for this.

## Commit messages

If you used an AI assistant to write the change, add a co-author trailer. We're transparent about it:

```
Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

Hand-written commits don't need any trailer. Mixed AI+human is fine either way.

## License

By contributing, you agree your changes ship under the same MIT license as the rest of the project. No CLA, no separate signing.
