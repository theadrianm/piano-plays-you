#!/usr/bin/env python3
"""Servo calibration for the glove.

Interactive, in a real terminal window (not through the Claude Code prompt):

    .venv/bin/python calibrate.py

      1-5     pick a finger              space   switch between its LOCK and RELEASE angle
      a / d   nudge -1 / +1              A / D   nudge -10 / +10      (arrow keys work too)
      s       save this finger to the glove's flash
      t       save, then play the finger once (release, press window, lock again)
      u       undo this finger's unsaved edits
      g       every finger to its lock angle      R   every finger to its release angle
      q       quit (locks everything)

One-shot forms, fine from any prompt:

    .venv/bin/python calibrate.py show
    .venv/bin/python calibrate.py move 2 35        # hold the index servo at 35 degrees to look at it
    .venv/bin/python calibrate.py move all 30      # hold every servo at 30 (re-seating horns: see README)
    .venv/bin/python calibrate.py set 2 35 120     # index: lock 35, release 120, saved to flash
    .venv/bin/python calibrate.py lock             # everything back to lock

LOCK is the angle where the badge reel is blocked (the finger can't curl),
RELEASE the angle where it runs free. The values live in the glove's flash and
survive reboots; play.py never changes them.

Angles run from -15 to 195, a little past the usual 0..180, for a horn that sits
one spline tooth off. A servo that buzzes has hit its end stop: back off 2-3
degrees, or move the horn.
"""
from __future__ import annotations

import re
import select
import sys
import termios
import time
import tty

from play import Glove, GloveError, colour, find_port

NAMES = ["thumb", "index", "middle", "ring", "pinky"]
MIN_ANGLE, MAX_ANGLE = -15, 195          # the firmware's limits: a little past both ends
ANGLE_RE = re.compile(r"(thumb|index|middle|ring|pinky) (-?\d+)/(-?\d+)")
NUDGE = {"a": -1, "d": 1, "A": -10, "D": 10, "left": -1, "right": 1, "down": -10, "up": 10}
HELP = (" 1-5 finger | space lock/release | a/d ±1  A/D ±10 (arrows too) | s save | t save+play"
        " | u undo | g all lock | R all release | q quit")


def read_angles(glove):
    """[[lock, release] per finger] from the glove's C command."""
    lines = []
    glove.cmd("C", capture=lines)
    found = {}
    for line in lines:
        for m in ANGLE_RE.finditer(line):
            found[NAMES.index(m.group(1))] = [int(m.group(2)), int(m.group(3))]
    if len(found) != 5:
        raise GloveError("couldn't read the calibration table from the glove")
    return [found[i] for i in range(5)]


def table(saved, edit, finger=None, which=None):
    rows = ["   finger      lock   release"]
    for i, name in enumerate(NAMES):
        cells = []
        for w in (0, 1):
            cell = f"{edit[i][w]:>3}"
            cells.append(colour(f"[{cell}]", "1;32") if (i == finger and w == which) else f" {cell} ")
        dirty = "*" if edit[i] != saved[i] else " "
        mark = ">" if i == finger else " "
        rows.append(f" {mark} {i + 1} {name:<7}  {cells[0]}    {cells[1]}  {dirty}")
    return "\n".join(rows)


class RawKeys:
    """One keypress at a time, arrows decoded, terminal restored on exit."""

    def __enter__(self):
        self.fd = sys.stdin.fileno()
        self.old = termios.tcgetattr(self.fd)
        tty.setcbreak(self.fd)
        return self

    def __exit__(self, *exc):
        termios.tcsetattr(self.fd, termios.TCSADRAIN, self.old)

    def get(self):
        ch = sys.stdin.read(1)
        if ch != "\x1b":
            return ch
        if select.select([sys.stdin], [], [], 0.05)[0]:
            seq = sys.stdin.read(2)
            return {"[A": "up", "[B": "down", "[C": "right", "[D": "left"}.get(seq, "esc")
        return "esc"


def interactive(glove):
    saved = read_angles(glove)
    edit = [list(v) for v in saved]
    finger, which = 0, 0
    status = "angles read from the glove"

    def hold(i, angle):
        glove.cmd(f"S {i + 1} {angle}")

    def save(i):
        glove.cmd(f"C {i + 1} {edit[i][0]} {edit[i][1]}")
        saved[i] = list(edit[i])

    hold(finger, edit[finger][which])
    with RawKeys() as keys:
        while True:
            dirty = [NAMES[i] for i in range(5) if edit[i] != saved[i]]
            print("\x1b[2J\x1b[H", end="")
            print(colour("Glove servo calibration", "1") + "    LOCK = reel blocked, RELEASE = reel runs free\n")
            print(table(saved, edit, finger, which))
            print(f"\n editing {colour(NAMES[finger], '1')} {'LOCK' if which == 0 else 'RELEASE'}:"
                  f" servo held at {edit[finger][which]}°" + (f"    unsaved: {', '.join(dirty)}" if dirty else ""))
            print(f" {status}\n\n{HELP}")
            key = keys.get()
            try:
                if key in ("1", "2", "3", "4", "5"):
                    hold(finger, edit[finger][0])          # the finger we leave rests at its (edited) lock
                    finger, which = int(key) - 1, 0
                    hold(finger, edit[finger][which])
                    status = ""
                elif key == " ":
                    which ^= 1
                    hold(finger, edit[finger][which])
                    status = ""
                elif key in NUDGE:
                    edit[finger][which] = max(MIN_ANGLE, min(MAX_ANGLE, edit[finger][which] + NUDGE[key]))
                    hold(finger, edit[finger][which])
                    status = ""
                elif key == "s":
                    save(finger)
                    hold(finger, edit[finger][which])
                    status = f"saved {NAMES[finger]}: lock {edit[finger][0]}, release {edit[finger][1]}"
                elif key == "t":
                    save(finger)
                    glove.cmd(f"P {finger + 1} 500")
                    time.sleep(1.0)
                    hold(finger, edit[finger][which])
                    status = f"saved and played {NAMES[finger]}: lock {edit[finger][0]}, release {edit[finger][1]}"
                elif key == "u":
                    edit[finger] = list(saved[finger])
                    hold(finger, edit[finger][which])
                    status = f"{NAMES[finger]} back to its saved values"
                elif key == "g":
                    for i in range(5):
                        hold(i, edit[i][0])
                    status = "every finger at its lock angle"
                elif key == "R":
                    for i in range(5):
                        hold(i, edit[i][1])
                    status = "every finger at its release angle"
                elif key in ("q", "esc", "\x04"):
                    if dirty:
                        print(f"\n unsaved: {', '.join(dirty)}.  y = save them and quit,  n = quit without saving,  other = stay")
                        answer = keys.get()
                        if answer == "y":
                            for i in range(5):
                                if edit[i] != saved[i]:
                                    save(i)
                        elif answer != "n":
                            status = "still here"
                            continue
                    break
            except GloveError as e:
                status = colour(str(e), "1;31")
    print("\n" + table(saved, saved))
    print("\n all servos locked; these values are in the glove's flash.")


def finger_arg(s):
    if s not in ("1", "2", "3", "4", "5"):
        sys.exit(f"finger must be 1-5 (1 = thumb), not {s!r}")
    return int(s)


def angle_arg(s):
    try:
        v = int(s)
    except ValueError:
        v = -1
    if not MIN_ANGLE <= v <= MAX_ANGLE:
        sys.exit(f"angle must be {MIN_ANGLE}..{MAX_ANGLE}, not {s!r}")
    return v


def main():
    args = sys.argv[1:]
    if args and args[0] in ("-h", "--help"):
        print(__doc__)
        return
    port = None
    if len(args) >= 2 and args[0] == "--port":
        port, args = args[1], args[2:]
    mode = args[0] if args else "interactive"
    if mode == "interactive" and not sys.stdin.isatty():
        sys.exit("Interactive calibration needs a real terminal window. Open Terminal and run it there,\n"
                 "or use the one-shot forms: show | move <finger> <angle> | set <finger> <lock> <release> | lock")

    glove = Glove(port or find_port())
    try:
        if mode == "interactive":
            interactive(glove)
        elif mode == "show":
            saved = read_angles(glove)
            print(table(saved, saved))
        elif mode == "move" and len(args) == 3:
            fingers = [1, 2, 3, 4, 5] if args[1] in ("all", "*") else [finger_arg(c) for c in args[1]]
            angle = angle_arg(args[2])
            for f in fingers:
                glove.cmd(f"S {f} {angle}")
            held = ", ".join(NAMES[f - 1] for f in fingers)
            print(f"{held} held at {angle}°. Stays there until 'lock', a song, or the next time a program opens the port.")
            return                                   # leave the hold in place
        elif mode == "set" and len(args) == 4:
            f, lock, release = finger_arg(args[1]), angle_arg(args[2]), angle_arg(args[3])
            print(glove.cmd(f"C {f} {lock} {release}"))
            glove.cmd(f"L {f}")
        elif mode == "lock":
            pass
        else:
            sys.exit(__doc__)
        glove.cmd("X")
    except KeyboardInterrupt:
        print()
        try:
            glove.cmd("X")
        except GloveError:
            pass
    except GloveError as e:
        sys.exit(colour(f"glove: {e}", "1;31"))
    finally:
        glove.close()


if __name__ == "__main__":
    main()
