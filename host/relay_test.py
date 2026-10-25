#!/usr/bin/env python3
"""Click test for the two relays (bring-up step 4).

    python relay_test.py --yes            # TENS unplugged or off: arm, pulse each relay, disarm

Each relay gets a one-second pulse (listen for the click, watch its IN LED on
the relay board, hold a multimeter in continuity mode across its two screw
terminals: beep only while the LED is on), then six quick alternating clicks.
The glove is disarmed again at the end, and on Ctrl-C.

Do this with the TENS unplugged from the relay wires or switched off: a closed
relay connects whatever is wired through it.
"""
from __future__ import annotations

import argparse
import sys
import time

from play import Glove, GloveError, colour, find_port


def relay_state(glove):
    """Ask ? and return the relay part of the ems status line."""
    glove.ser.write(b"?\n")
    state = "?"
    deadline = time.monotonic() + 1.5
    while time.monotonic() < deadline:
        line = glove.readline()
        if line is None:
            continue
        if line.startswith("# ems:"):
            state = line.split("|", 2)[-1].strip()
        if line.startswith(("ok", "err")):
            break
    return state


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: auto)")
    ap.add_argument("--ms", type=int, default=1000, help="length of the long pulse (default 1000)")
    ap.add_argument("--yes", action="store_true", help="confirm the TENS is unplugged or off")
    args = ap.parse_args()

    if not args.yes:
        if not sys.stdin.isatty():
            sys.exit("Confirm the TENS is unplugged or off by running with --yes")
        input("TENS unplugged from the relay wires or switched off? Enter to continue, Ctrl-C to stop... ")

    glove = Glove(args.port or find_port())
    armed = False
    try:
        print(glove.cmd("A 1"))
        armed = True
        for r in (1, 2):
            print(colour(f"\nrelay {r}: {args.ms} ms pulse. Listen for the click, watch IN{r}'s LED.", "1"))
            print("  ", glove.cmd(f"K {r} {args.ms}"))
            time.sleep(0.3)
            print("   during:", relay_state(glove))
            time.sleep(args.ms / 1000.0)
            print("   after: ", relay_state(glove))
        print(colour("\nsix quick clicks, alternating", "1"))
        for _ in range(3):
            glove.cmd("K 1 250")
            time.sleep(0.5)
            glove.cmd("K 2 250")
            time.sleep(0.5)
        print("\nIf you heard nothing and no LED lit: the relay board has no 5 V (its VCC comes from the servo rail,"
              " so the battery must be on). LED but no click: the JD-VCC jumper or coil supply.")
    except KeyboardInterrupt:
        print()
    except GloveError as e:
        print(colour(f"glove: {e}", "1;31"))
    finally:
        if armed:
            try:
                print(glove.cmd("A 0"))
            except GloveError:
                pass
        glove.close()


if __name__ == "__main__":
    main()
