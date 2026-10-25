"""Song files for the Piano Plays You glove, and the schedule that plays them.

A song is a small text file:

    title: Ode to Joy
    bpm: 100
    position: C4        # the key under the thumb (right hand): the five-finger position
    hand: right         # right = thumb on the lowest key, left = thumb on the highest

    E4 E4 F4 G4 | G4 F4 E4 D4
    C4 C4 D4 E4 | E4:1.5 D4:0.5 D4:2

Tokens are separated by whitespace. A token is <pitch>[:beats]:
    pitch   a note in the five-finger position: C4 D4 E4 F4 G4 for position C4
            or finger numbers 1-5 (1 = thumb); several digits make a chord: 13
            or a rest: r, rest, -
    beats   how long, default 1, decimals allowed (0.5, 1.5)
    chord   [C4 E4]:2
Bar lines "|" are ignored, and so is a comment: a "#" that starts a word runs to
the end of the line (F#4 is still a sharp).

Headers (all optional): title, bpm (default 90), position (default C4),
hand (default right), map (an explicit note list: "map: C4=1 D4=2 E4=3 F4=4 G4=5",
which overrides position and hand).
"""
from __future__ import annotations

import re
from dataclasses import dataclass

DEFAULT_BPM = 90
DEFAULT_POSITION = "C4"
HEADER_KEYS = {"title", "bpm", "position", "hand", "map"}
WHITE_KEYS = ["C", "D", "E", "F", "G", "A", "B"]
SEMITONES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
PITCH_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
NOTE_RE = re.compile(r"^([A-Ga-g])([#b]?)(-?\d)$")
HEADER_RE = re.compile(r"^([A-Za-z_]+)\s*:\s*(.*)$")
COMMENT_RE = re.compile(r"(^|\s)#.*$")
REST_WORDS = {"r", "rest", "-", "_"}
FINGER_NAMES = {1: "thumb", 2: "index", 3: "middle", 4: "ring", 5: "pinky"}

# Firmware limits (see firmware/glove/glove.ino).
FIRMWARE_MIN_PRESS_MS = 30
FIRMWARE_MAX_PRESS_MS = 1500


class SongError(ValueError):
    pass


@dataclass
class Event:
    start: float              # beats from the start of the song
    beats: float
    fingers: frozenset        # of int 1..5
    label: str                # what was written, e.g. "C4" or "[C4 E4]"


@dataclass
class Song:
    title: str
    bpm: float
    position: str
    hand: str
    fingering: dict           # semitone -> finger
    events: list
    total_beats: float

    def seconds(self, bpm=None):
        return self.total_beats * 60.0 / (bpm or self.bpm)

    def shortest_beats(self):
        return min((e.beats for e in self.events), default=1.0)


@dataclass
class Step:
    t: float                  # seconds from the start of the song
    fingers: str              # "13"
    press_ms: int
    label: str
    beats: float


def note_semitone(name):
    m = NOTE_RE.match(name)
    if not m:
        return None
    letter, acc, octave = m.group(1).upper(), m.group(2), int(m.group(3))
    return octave * 12 + SEMITONES[letter] + (1 if acc == "#" else -1 if acc == "b" else 0)


def semitone_name(s):
    return f"{PITCH_NAMES[s % 12]}{s // 12}"


def position_fingering(position, hand):
    """Five white keys going up from `position`, thumb on the lowest for the right hand."""
    m = NOTE_RE.match(position)
    if not m or m.group(2):
        raise SongError(f"position must be a white key like C4 or G4, not {position!r}")
    letter, octave = m.group(1).upper(), int(m.group(3))
    idx = WHITE_KEYS.index(letter)
    keys = []
    for k in range(5):
        w = idx + k
        keys.append((octave + w // 7) * 12 + SEMITONES[WHITE_KEYS[w % 7]])
    fingers = range(1, 6) if hand == "right" else range(5, 0, -1)
    return dict(zip(keys, fingers))


def parse_fingering_map(text):
    out = {}
    for item in text.split():
        note, sep, finger = item.partition("=")
        s = note_semitone(note)
        if not sep or s is None or finger not in ("1", "2", "3", "4", "5"):
            raise SongError(f"map entries look like C4=1, not {item!r}")
        out[s] = int(finger)
    if not out:
        raise SongError("map: is empty")
    return out


def fingering_text(fingering):
    return " ".join(f"{semitone_name(s)}={f}" for s, f in sorted(fingering.items()))


def tokenize(line, where):
    """Whitespace-separated tokens, with a bracketed chord kept as one token."""
    tokens = []
    words = line.split()
    i = 0
    while i < len(words):
        word = words[i]
        if word.startswith("["):
            chord = word
            while "]" not in chord:
                i += 1
                if i >= len(words):
                    raise SongError(f"{where}: chord {chord!r} is missing its ]")
                chord += " " + words[i]
            tokens.append(chord)
        elif "[" in word or "]" in word:
            raise SongError(f"{where}: stray bracket in {word!r}")
        else:
            tokens.append(word)
        i += 1
    return tokens


def split_beats(tok, where):
    """'C4:2' -> ('C4', 2.0); '[C4 E4]:1.5' -> ('[C4 E4]', 1.5); 'C4' -> ('C4', 1.0)."""
    if tok.startswith("["):
        close = tok.find("]")
        if close < 0:
            raise SongError(f"{where}: chord {tok!r} is missing its ]")
        pitch, rest = tok[: close + 1], tok[close + 1 :]
        if rest and not rest.startswith(":"):
            raise SongError(f"{where}: don't understand {tok!r}")
        beats_s = rest[1:]
    else:
        pitch, _, beats_s = tok.partition(":")
    if not beats_s:
        return pitch, 1.0
    try:
        beats = float(beats_s)
    except ValueError:
        raise SongError(f"{where}: beats in {tok!r} must be a number like 2 or 0.5") from None
    if beats <= 0 or beats > 16:
        raise SongError(f"{where}: beats in {tok!r} must be between 0 and 16")
    return pitch, beats


def resolve_pitch(pitch, fingering, where):
    """A pitch token -> frozenset of fingers, or None for a rest."""
    if pitch.startswith("["):
        fingers = set()
        for part in pitch[1:-1].split():
            got = resolve_pitch(part, fingering, where)
            if got is None:
                raise SongError(f"{where}: a rest can't be part of the chord {pitch!r}")
            fingers |= got
        if not fingers:
            raise SongError(f"{where}: the chord {pitch!r} is empty")
        return frozenset(fingers)
    if pitch.lower() in REST_WORDS:
        return None
    if pitch.isdigit():
        if any(c not in "12345" for c in pitch):
            raise SongError(f"{where}: fingers are 1-5 (1 = thumb), not {pitch!r}")
        return frozenset(int(c) for c in pitch)
    s = note_semitone(pitch)
    if s is None:
        raise SongError(f"{where}: don't understand {pitch!r} (want a note like E4, a finger 1-5, or r)")
    if s not in fingering:
        keys = " ".join(semitone_name(k) for k in sorted(fingering))
        raise SongError(
            f"{where}: {semitone_name(s)} is outside the five-finger position ({keys}). "
            "Move the note, change position:, or give an explicit map:"
        )
    return frozenset([fingering[s]])


def parse_song(text, source="<text>"):
    headers = {}
    body = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = COMMENT_RE.sub("", raw).strip()   # "# ..." starts a comment, F#4 keeps its sharp
        if not line:
            continue
        m = HEADER_RE.match(line)
        if m and m.group(1).lower() in HEADER_KEYS:
            headers[m.group(1).lower()] = m.group(2).strip()
            continue
        body.append((lineno, line))

    title = headers.get("title") or "untitled"
    try:
        bpm = float(headers.get("bpm", DEFAULT_BPM))
    except ValueError:
        raise SongError(f"{source}: bpm must be a number") from None
    if not 20 <= bpm <= 300:
        raise SongError(f"{source}: bpm {bpm:g} is out of range (20-300)")
    position = headers.get("position", DEFAULT_POSITION)
    hand = headers.get("hand", "right").lower()
    if hand not in ("right", "left"):
        raise SongError(f"{source}: hand must be right or left, not {hand!r}")
    if "map" in headers:
        fingering = parse_fingering_map(headers["map"])
    else:
        fingering = position_fingering(position, hand)

    events = []
    cursor = 0.0
    for lineno, line in body:
        where = f"{source}:{lineno}"
        for tok in tokenize(line, where):
            if set(tok) <= {"|"}:
                continue
            pitch, beats = split_beats(tok, where)
            fingers = resolve_pitch(pitch, fingering, where)
            if fingers is not None:
                events.append(Event(start=cursor, beats=beats, fingers=fingers, label=pitch))
            cursor += beats
    if not events:
        raise SongError(f"{source}: no notes found")
    return Song(title, bpm, position, hand, fingering, events, cursor)


def parse_song_file(path):
    with open(path, encoding="utf-8") as fh:
        return parse_song(fh.read(), source=str(path))


def fingers_text(fingers):
    return "".join(str(f) for f in sorted(fingers))


def schedule(song, bpm=None, gap_ms=150, margin_ms=20, min_press_ms=120, max_press_ms=FIRMWARE_MAX_PRESS_MS):
    """Turn a song into timed P commands.

    Each note's EMS press lasts the note minus the gap the next finger needs to
    lift (gap_ms) minus a little margin, capped to what the firmware allows.
    Returns (steps, warnings).
    """
    bpm = bpm or song.bpm
    beat = 60.0 / bpm
    floor_ms = min_press_ms + gap_ms + margin_ms
    steps, warnings = [], []
    too_short = 0
    for ev in song.events:
        dur_ms = ev.beats * beat * 1000.0
        press = dur_ms - gap_ms - margin_ms
        if press < min_press_ms:
            too_short += 1
            press = min_press_ms
        press = max(FIRMWARE_MIN_PRESS_MS, min(press, max_press_ms))
        steps.append(Step(t=ev.start * beat, fingers=fingers_text(ev.fingers), press_ms=int(round(press)),
                          label=ev.label, beats=ev.beats))
    if too_short:
        ok_bpm = int(song.shortest_beats() * 60000.0 / floor_ms)
        warnings.append(
            f"{too_short} note(s) are shorter than the {floor_ms:.0f} ms the EMS needs at {bpm:g} BPM; "
            f"they get {min_press_ms} ms and the glove will run late. Try --bpm {ok_bpm} or slower."
        )
    return steps, warnings
