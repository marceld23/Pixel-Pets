#!/usr/bin/env python3
"""Long-running Serial listener — appends every byte to capture.log.

Used together with `shot all`: enable auto-dump on the device, run this
listener in the background, navigate at the device for as long as you
need, then stop the listener (Ctrl-C / kill).

DTR/RTS are pinned low on open so the ESP32 is not auto-reset.
"""
import argparse
import os
import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write("error: pyserial not installed.  cd tools && uv sync\n")
    sys.exit(1)


def main(argv):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", default=os.environ.get("PIXELPETS_PORT", "COM3"))
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--log", default="capture.log")
    args = p.parse_args(argv[1:])

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.5
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False

    sys.stderr.write(f"listening on {args.port} @ {args.baud} → {args.log}\n")
    sys.stderr.flush()

    try:
        with open(args.log, "ab") as f:
            while True:
                chunk = ser.read(4096)
                if chunk:
                    f.write(chunk)
                    f.flush()
                else:
                    # heartbeat tick — keeps loop responsive to signals
                    time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
