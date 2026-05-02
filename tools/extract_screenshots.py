#!/usr/bin/env python3
"""Extract screenshots from a `pio device monitor` log into PNG files.

Usage:
    pio device monitor -p COM5 -b 115200 > capture.log
    python tools/extract_screenshots.py capture.log out/

Or stream live:
    pio device monitor -p COM5 -b 115200 | python tools/extract_screenshots.py - out/

The firmware writes blocks of the form:

    --SCREENSHOT-BEGIN name=<name> w=<W> h=<H> fmt=rgb565le bytes=<N>
    <base64 RGB565 little-endian>
    --SCREENSHOT-END

This script collects each block, decodes the RGB565 pixels and writes them
as `<name>_<seq>.png` (the seq counter prevents overwrites when you take
multiple shots of the same screen).

Requires Pillow:  pip install pillow
"""
import base64
import os
import re
import sys
from collections import defaultdict
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("error: Pillow not installed.  pip install pillow\n")
    sys.exit(1)


HEADER_RE = re.compile(
    r"^--SCREENSHOT-BEGIN\s+name=(\S+)\s+w=(\d+)\s+h=(\d+)\s+fmt=(\S+)\s+bytes=(\d+)"
)


def rgb565_to_rgb888(buf: bytes, w: int, h: int, big_endian: bool) -> bytes:
    """Decode RGB565 (2 bytes/pixel) into 24-bit RGB.

    `big_endian=True` matches M5Canvas / LovyanGFX sprite buffers on
    M5Stack ILI934x devices (the panel's native wire order).
    """
    out = bytearray(w * h * 3)
    for i in range(w * h):
        b0 = buf[i * 2]
        b1 = buf[i * 2 + 1]
        if big_endian:
            v = (b0 << 8) | b1
        else:
            v = (b1 << 8) | b0
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        # Expand to 8-bit per channel; replicate top bits to fill the LSBs
        # so 0x1F → 0xFF cleanly.
        out[i * 3] = (r << 3) | (r >> 2)
        out[i * 3 + 1] = (g << 2) | (g >> 4)
        out[i * 3 + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def save_screenshot(out_dir: Path, name: str, w: int, h: int, raw: bytes,
                    fmt: str, counters: dict) -> Path:
    counters[name] += 1
    seq = counters[name]
    filename = f"{name}_{seq:02d}.png"
    rgb = rgb565_to_rgb888(raw, w, h, big_endian=(fmt == "rgb565be"))
    img = Image.frombytes("RGB", (w, h), rgb)
    out_path = out_dir / filename
    img.save(out_path)
    return out_path


def parse_stream(lines, out_dir: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    counters = defaultdict(int)
    in_block = False
    header = None
    b64_chunks: list[str] = []
    saved = 0

    for raw_line in lines:
        line = raw_line.rstrip("\r\n")
        if not in_block:
            m = HEADER_RE.match(line)
            if m:
                name = m.group(1).replace("/", "_")
                w = int(m.group(2))
                h = int(m.group(3))
                fmt = m.group(4)
                nbytes = int(m.group(5))
                if fmt not in ("rgb565be", "rgb565le"):
                    sys.stderr.write(
                        f"warning: unknown format '{fmt}' for {name}, skipping\n"
                    )
                    continue
                header = (name, w, h, nbytes, fmt)
                b64_chunks = []
                in_block = True
        else:
            if line == "--SCREENSHOT-END":
                if header is not None:
                    name, w, h, nbytes, fmt = header
                    try:
                        decoded = base64.b64decode("".join(b64_chunks))
                    except Exception as e:  # pragma: no cover
                        sys.stderr.write(f"error: base64 decode for {name}: {e}\n")
                        decoded = b""
                    if len(decoded) >= nbytes:
                        out_path = save_screenshot(
                            out_dir, name, w, h, decoded[:nbytes], fmt, counters
                        )
                        sys.stderr.write(f"saved {out_path}  ({w}x{h})\n")
                        saved += 1
                    else:
                        sys.stderr.write(
                            f"warning: short decode for {name} "
                            f"({len(decoded)}/{nbytes} bytes)\n"
                        )
                in_block = False
                header = None
                b64_chunks = []
            else:
                b64_chunks.append(line)

    return saved


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        sys.stderr.write(
            "usage: extract_screenshots.py <capture.log|-> <out_dir>\n"
        )
        return 1
    src = argv[1]
    out_dir = Path(argv[2]) if len(argv) > 2 else Path("screenshots")

    if src == "-":
        n = parse_stream(sys.stdin, out_dir)
    else:
        with open(src, "r", encoding="utf-8", errors="replace") as f:
            n = parse_stream(f, out_dir)
    sys.stderr.write(f"\n{n} screenshot(s) extracted into {out_dir}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
