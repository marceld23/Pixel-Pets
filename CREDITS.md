<p align="center">
  <img src="assets/logo.jpg" alt="Pixel Pets logo" width="180">
</p>

# Credits

Pixel Pets is a father-and-son project.

## Justus Dütscher (10 years old)

The pet's chief designer, product owner and head of QA.

Justus brought the ideas — what the pet should *feel* like, how it should react when you poke it on the cheek vs. the forehead, which mini-games belong in which scene, what kind of treats Pip should throw, what the moods should look like. He tested every build until it was right, picked the visuals that stayed and the ones that got thrown out, and made the kid-experience calls (is it too easy? too loud? too dim at night?) that would have been impossible to make from an adult's perspective.

If the pet feels alive, that's Justus.

## Marcel Dütscher (Dad)

The translator and technical wingman.

My job was to take Justus's ideas — usually delivered at high speed, often half-acted-out, occasionally mid-snack — and turn them into briefs the AI could execute. Plus the bits that don't fit on a kid's whiteboard: hardware quirks, partition tables, ESP-NOW packet layouts, the occasional debugging session.

The first beta was finished on a Sunday afternoon, sitting next to Justus on the sofa. The boring polish (CI, partition tables, NVS migrations, README rewrites) was Papa's work in the evenings.

## The AI

Every line of firmware in this project was written by AI assistants — primarily **Claude** (Anthropic) — under our direction. We supplied the goals, the design constraints and the running commentary; the AI did the actual typing.

Each commit is co-authored with the model that wrote it. Run

```bash
git log --format="%an %ae" | sort -u
```

to see the breakdown.

## In-app credits

The pet itself shows a credits screen via **Settings → Credits**. It mirrors this file but in the kid's tone — what the pet thinks of its makers, basically.

## Third-party software

Pixel Pets is MIT-licensed (see [`LICENSE`](LICENSE)). Binaries built from the `cores3` / `core2` / `visu` envs link the GPL-3.0 [`schreibfaul1/ESP32-audioI2S`](https://github.com/schreibfaul1/ESP32-audioI2S) for web radio decoding, so distributed binaries inherit GPL-3.0 obligations. The `pip` env stays MIT all the way through.
