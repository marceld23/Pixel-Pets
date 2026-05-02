#!/usr/bin/env python3
"""Send a `shot` command to the firmware and capture its response.

Workflow:
    1. Flash the device with `pio run -e <target>-shots -t upload`.
    2. Navigate at the device to the screen you want.
    3. From the host, run:

        uv run --project tools tools/shot.py --port COM3

       (default port is read from $PIXELPETS_PORT, fallback COM3)

    4. Repeat per screen. Output is appended to `capture.log`.
    5. When done, decode:

        uv run --project tools tools/extract_screenshots.py capture.log screenshots/

Commands accepted by the firmware (pass via --cmd, default "shot"):
    shot          dump current screen once
    shot all      auto-dump on every screen-name change
    shot stop     disable auto-dump
"""
import argparse
import os
import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write(
        "error: pyserial not installed.  cd tools && uv sync\n"
    )
    sys.exit(1)


DEFAULT_BAUD = 115200
# A full 320x240 RGB565 dump base64-encoded over 115200 baud takes ~18 s.
# `shot animals` chains three back-to-back, so the default timeout has to
# cover the whole cycle plus settle time.
DEFAULT_TIMEOUT = 90.0
# Stop reading when no bytes have arrived for this long. Long enough to
# bridge inter-dump gaps in the animals cycle (~350 ms), short enough to
# not idle-spin after a single shot.
QUIET_EXIT_S = 3.0


def capture(port: str, cmd: str, log_path: str, timeout: float) -> int:
    # On the M5Stack CH9102 USB-Serial, DTR/RTS map to EN/IO0 — the default
    # pyserial open pulses both high which auto-resets the ESP32. We pin
    # them low *before* opening so the device keeps running undisturbed.
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = DEFAULT_BAUD
    ser.timeout = 0.3
    ser.dtr = False
    ser.rts = False
    ser.open()
    # Belt-and-braces: re-assert after open in case the OS toggled briefly.
    ser.dtr = False
    ser.rts = False
    try:
        # Toss any pending serial chatter so the response window is clean.
        ser.reset_input_buffer()

        ser.write((cmd + "\n").encode("ascii"))
        ser.flush()

        deadline = time.time() + timeout
        buf = bytearray()
        last_data = time.time()
        in_dump = False
        completed_dumps = 0

        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf.extend(chunk)
                last_data = time.time()
                # Track dump boundaries so we know when we are mid-stream
                # vs. quiescent. Counts each fully terminated dump.
                if b"--SCREENSHOT-BEGIN" in chunk:
                    in_dump = True
                if in_dump and b"--SCREENSHOT-END" in chunk:
                    completed_dumps += chunk.count(b"--SCREENSHOT-END")
                    in_dump = False
            else:
                # Exit when the device has been quiet long enough — and
                # never mid-dump (so we don't truncate base64).
                if not in_dump and time.time() - last_data > QUIET_EXIT_S:
                    break

        with open(log_path, "ab") as f:
            f.write(buf)

        if completed_dumps > 0:
            sys.stderr.write(
                f"ok: captured {completed_dumps} screenshot(s) "
                f"({len(buf)} bytes appended to {log_path})\n"
            )
            return 0
        if in_dump:
            sys.stderr.write(
                f"warn: dump in progress at timeout ({timeout:.0f}s) "
                f"({len(buf)} bytes saved)\n"
            )
            return 2
        if buf:
            sys.stderr.write(
                f"info: command '{cmd}' acknowledged, no dump "
                f"({len(buf)} bytes saved)\n"
            )
            return 0
        sys.stderr.write(
            f"warn: no response from {port}; is the device booted and running "
            f"the *-shots firmware?\n"
        )
        return 3
    finally:
        ser.close()


def main(argv):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", default=os.environ.get("PIXELPETS_PORT", "COM3"),
                   help="serial port (default: $PIXELPETS_PORT or COM3)")
    p.add_argument("--cmd", default="shot",
                   help="command to send (default: 'shot')")
    p.add_argument("--log", default="capture.log",
                   help="append response to this file (default: capture.log)")
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                   help=f"max seconds to wait for response (default: {DEFAULT_TIMEOUT})")
    args = p.parse_args(argv[1:])
    return capture(args.port, args.cmd, args.log, args.timeout)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
