#!/usr/bin/env python3
"""Play a song file on the glove over USB serial.

    python play.py songs/twinkle.txt            # servos only (EMS stays disarmed)
    python play.py songs/twinkle.txt --arm      # the real thing: relays live
    python play.py --show songs/twinkle.txt     # print the schedule, no glove needed
    python play.py --repl                       # type raw glove commands (calibration)
    python play.py --list-ports

Needs pyserial:  pip install pyserial
Close the Arduino Serial Monitor first; only one program can hold the port.
"""
from __future__ import annotations

import argparse
import sys
import time

from songfile import SongError, fingering_text, parse_song_file, schedule

BAUD = 115200
PORT_HINTS = ("usbserial", "slab", "cp210", "ch340", "wch", "ttyusb", "ttyacm", "uart")

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None


class GloveError(Exception):
    pass


def colour(text, code):
    if sys.stdout.isatty():
        return f"\033[{code}m{text}\033[0m"
    return text


class Glove:
    """One command in, one 'ok'/'err' line out. '#' and '!' lines are shown as they arrive."""

    def __init__(self, port, verbose=False):
        if serial is None:
            sys.exit("pyserial is not installed. Run: pip install pyserial")
        self.verbose = verbose
        try:
            self.ser = serial.Serial(port, BAUD, timeout=0.1)
        except serial.SerialException as e:
            sys.exit(f"Can't open {port}: {e}\nIs the Arduino Serial Monitor still open? Close it and retry.")
        print(f"Opened {port}, waiting for the glove to boot...")
        # Opening the port resets the ESP32. Its banner ends with "# ready".
        if not self.wait_for("# ready", 6.0):
            self.ser.reset_input_buffer()
            try:
                self.cmd("?")
            except GloveError:
                sys.exit("Nothing answered. Is the glove firmware uploaded and this the right port? (--list-ports)")

    def readline(self):
        raw = self.ser.readline()
        if not raw:
            return None
        return raw.decode("utf-8", "replace").strip()

    def show(self, line):
        if line.startswith("!"):
            print(colour(f"  {line}", "1;31"))
        elif line.startswith("#"):
            if self.verbose or not line.startswith("# ems"):
                print(colour(f"  {line}", "2"))
        elif self.verbose and line:
            print(colour(f"  {line}", "2"))

    def wait_for(self, prefix, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.readline()
            if line is None:
                continue
            self.show(line)
            if line.startswith(prefix):
                return True
        return False

    def drain(self):
        while True:
            line = self.readline()
            if line is None:
                return
            self.show(line)

    def cmd(self, line, timeout=1.5):
        self.ser.write((line.strip() + "\n").encode())
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            reply = self.readline()
            if reply is None:
                continue
            if reply.startswith("ok"):
                return reply
            if reply.startswith("err"):
                raise GloveError(f"{line!r} -> {reply}")
            self.show(reply)
        raise GloveError(f"no reply to {line!r}")

    def close(self):
        self.ser.close()


def find_port():
    if serial is None:
        sys.exit("pyserial is not installed. Run: pip install pyserial")
    found = []
    for p in list_ports.comports():
        text = " ".join(filter(None, (p.device, p.description, p.manufacturer))).lower()
        if "bluetooth" in text or "debug-console" in text:
            continue
        if any(h in text for h in PORT_HINTS):
            found.append(p.device)
    if len(found) == 1:
        return found[0]
    if not found:
        sys.exit("No USB serial port found. Plug the ESP32 in, or pass --port. (--list-ports shows everything)")
    sys.exit("Several USB serial ports found, pass one with --port:\n  " + "\n  ".join(found))


def show_ports():
    for p in list_ports.comports():
        print(f"{p.device:<32} {p.description or ''}")


def print_schedule(song, steps, bpm):
    print(f"{song.title}  |  {bpm:g} BPM  |  {len(steps)} notes  |  {song.seconds(bpm):.1f} s  |  "
          f"keys {fingering_text(song.fingering)}")
    print(f"{'time':>7}  {'note':<10} {'fingers':<8} {'press':>6}")
    for s in steps:
        print(f"{s.t:7.2f}  {s.label:<10} {s.fingers:<8} {s.press_ms:>4} ms")


def play(args):
    try:
        song = parse_song_file(args.song)
    except (SongError, OSError) as e:
        sys.exit(f"song error: {e}")
    bpm = args.bpm or song.bpm
    steps, warnings = schedule(song, bpm=bpm, gap_ms=args.gap, min_press_ms=args.min_press)
    print_schedule(song, steps, bpm)
    for w in warnings:
        print(colour(f"warning: {w}", "33"))
    if args.show:
        return

    glove = Glove(args.port or find_port(), verbose=args.verbose)
    armed = False
    try:
        # Keep the firmware's timing in step with the schedule we just built.
        glove.cmd(f"T {args.lead} {args.relax} {args.gap}")
        glove.cmd("V 1" if args.verbose else "V 0")
        if args.arm:
            glove.cmd("A 1")
            armed = True
            print(colour("EMS ARMED. Relays are live.", "1;31"))
        else:
            print("EMS disarmed: servos only. Add --arm for the real thing.")
        if not args.now:
            input("Press Enter to play (Ctrl-C stops everything)... ")

        t0 = time.monotonic() + 0.5
        for step in steps:
            while True:
                remaining = t0 + step.t - time.monotonic()
                if remaining <= 0:
                    break
                time.sleep(min(remaining, 0.05))
            late = time.monotonic() - (t0 + step.t)
            glove.cmd(f"P {step.fingers} {step.press_ms}")
            flag = colour(f"  late {late * 1000:.0f} ms", "33") if late > 0.08 else ""
            print(f"{step.t:7.2f}  {step.label:<10} {step.fingers:<8} {step.press_ms:>4} ms{flag}")
        tail = (args.lead + steps[-1].press_ms + args.relax) / 1000.0 + 0.3
        time.sleep(tail)
        glove.drain()
        print("done")
    except KeyboardInterrupt:
        print()
        glove.cmd("X")
        print("stopped")
    except GloveError as e:
        print(colour(f"glove: {e}", "1;31"))
        try:
            glove.cmd("X")
        except GloveError:
            pass
    finally:
        if armed:
            try:
                glove.cmd("A 0")
            except GloveError:
                pass
        glove.close()


def repl(args):
    glove = Glove(args.port or find_port(), verbose=True)
    print("Type glove commands (? for help, quit to leave). Ctrl-C sends X.")
    try:
        while True:
            glove.drain()
            try:
                line = input("glove> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                glove.cmd("X")
                break
            if line.lower() in ("quit", "exit", "q"):
                break
            if not line:
                continue
            try:
                print(glove.cmd(line))
            except GloveError as e:
                print(colour(str(e), "1;31"))
    finally:
        glove.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("song", nargs="?", help="song file (see songs/)")
    ap.add_argument("--port", help="serial port, e.g. /dev/cu.usbserial-0001 (default: auto)")
    ap.add_argument("--arm", action="store_true", help="arm the EMS relays (default: servos only)")
    ap.add_argument("--now", action="store_true", help="start without waiting for Enter")
    ap.add_argument("--bpm", type=float, help="override the song's tempo")
    ap.add_argument("--lead", type=int, default=150, help="ms from servo release to EMS on (default 150)")
    ap.add_argument("--relax", type=int, default=50, help="ms from EMS off to servo lock (default 50)")
    ap.add_argument("--gap", type=int, default=150, help="ms the EMS stays off between notes (default 150)")
    ap.add_argument("--min-press", type=int, default=120, help="shortest useful EMS press in ms (default 120)")
    ap.add_argument("--show", action="store_true", help="print the schedule and exit")
    ap.add_argument("--repl", action="store_true", help="interactive glove console")
    ap.add_argument("--list-ports", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true", help="show every line the glove prints")
    args = ap.parse_args()

    if args.list_ports:
        if serial is None:
            sys.exit("pyserial is not installed. Run: pip install pyserial")
        show_ports()
    elif args.repl:
        repl(args)
    elif args.song:
        play(args)
    else:
        ap.print_help()


if __name__ == "__main__":
    main()
