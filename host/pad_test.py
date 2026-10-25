#!/usr/bin/env python3
"""Pad placement test: pulse the TENS relay over and over until you press Ctrl-C.

    python pad_test.py                 # relay 2, 1 s on, 1 s off
    python pad_test.py --on 500 --off 1500
    python pad_test.py --relay 1

Start with the TENS dial at zero, arm, then raise the dial a notch at a time
between pulses while you move the pads until the right fingers twitch.
The firmware caps one closure at 1.5 s and forces a rest after 2 s continuous,
so --on is limited to 1500 and the pulses always have gaps. Ctrl-C disarms.
"""
from __future__ import annotations

import argparse
import sys
import time

from play import Glove, GloveError, colour, find_port


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--relay", type=int, default=2, choices=(1, 2))
    ap.add_argument("--on", type=int, default=1000, help="ms closed per pulse, 30..1500 (default 1000)")
    ap.add_argument("--off", type=int, default=1000, help="ms open between pulses, at least 300 (default 1000)")
    args = ap.parse_args()
    if not 30 <= args.on <= 1500:
        sys.exit("--on must be 30..1500")
    if args.off < 300:
        sys.exit("--off must be at least 300")

    glove = Glove(args.port or find_port())
    armed = False
    try:
        print(glove.cmd("A 1"))
        armed = True
        print(colour(f"relay {args.relay}: {args.on} ms on, {args.off} ms off, until Ctrl-C. Dial up slowly.", "1;31"))
        n = 0
        while True:
            glove.cmd(f"K {args.relay} {args.on}")
            n += 1
            print(f"\rpulse {n}  ", end="", flush=True)
            time.sleep((args.on + args.off) / 1000.0)
            glove.drain()
    except KeyboardInterrupt:
        print()
    except GloveError as e:
        print(colour(f"\nglove: {e}", "1;31"))
    finally:
        if armed:
            try:
                print(glove.cmd("A 0"))
            except GloveError:
                pass
        glove.close()


if __name__ == "__main__":
    main()
