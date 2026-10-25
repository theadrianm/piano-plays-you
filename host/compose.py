#!/usr/bin/env python3
"""Ask Claude to arrange a song for the glove and write it as a song file.

    python compose.py "Twinkle Twinkle Little Star"
    python compose.py "Ode to Joy" --bpm 100 -o songs/ode.txt
    python compose.py "Happy Birthday" --print        # don't write, just show it

Then:  python play.py songs/twinkle-twinkle-little-star.txt --arm

Needs:  pip install anthropic   and   export ANTHROPIC_API_KEY=...
The result goes through the same parser play.py uses, so what comes out is playable.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

import songfile

try:
    import anthropic
except ImportError:
    anthropic = None

MODEL = "claude-opus-5-5"
SONG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "songs")

EXAMPLE = """title: Mary Had a Little Lamb
bpm: 96
position: C4
hand: right

E4 D4 C4 D4 | E4 E4 E4:2 | D4 D4 D4:2 | E4 G4 G4:2
E4 D4 C4 D4 | E4 E4 E4 E4 | D4 D4 E4 D4 | C4:4
"""

SYSTEM = f"""You arrange well-known songs for a one-handed haptic piano glove and write them as song files.

The glove plays with one hand in a fixed five-finger position: five neighbouring white keys, one finger each, thumb on the lowest key (right hand). It plays one note at a time; an occasional two-note chord is fine. Every note takes at least 320 ms of real time, so keep the tempo between 60 and 110 BPM and use quarter notes, eighth notes and longer. No sixteenth notes, no triplets.

Arrange the melody the way a first piano book does: choose the position so the tune fits in five white keys, transpose freely, and simplify any note that falls outside the position to a neighbouring note that is inside it. Keep the recognisable melody, 8 to 32 bars, melody only. If you do not know a song, write a short simple tune that fits the title rather than guessing at a real one.

Write the complete song file and nothing else: no explanation, no code fences.

Song file format:
{songfile.__doc__}
Example:

{EXAMPLE}"""


def strip_fences(text):
    text = text.strip()
    m = re.match(r"^```[^\n]*\n(.*?)\n```\s*$", text, re.S)
    return m.group(1) if m else text


def slug(title):
    return re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-") or "song"


def ask(client, messages):
    response = client.beta.messages.create(
        model=MODEL,
        max_tokens=4000,
        betas=["server-side-fallback-2026-07-01"],
        fallbacks="default",
        output_config={"effort": "high"},
        system=SYSTEM,
        messages=messages,
    )
    if response.stop_reason == "refusal":
        detail = response.stop_details.explanation if response.stop_details else ""
        sys.exit(f"Claude declined this one: {detail}")
    text = "".join(block.text for block in response.content if block.type == "text")
    if response.stop_reason == "max_tokens":
        sys.exit("The reply was cut off. Ask for a shorter arrangement.")
    return strip_fences(text)


def compose(title, bpm=None, position=None, bars=None):
    request = f"Song: {title}"
    if bpm:
        request += f"\nTempo: {bpm} BPM"
    if position:
        request += f"\nPosition: {position}"
    if bars:
        request += f"\nLength: about {bars} bars"
    client = anthropic.Anthropic()
    messages = [{"role": "user", "content": request}]
    text = ask(client, messages)
    for attempt in range(2):
        try:
            return text, songfile.parse_song(text, source="claude")
        except songfile.SongError as e:
            if attempt == 1:
                sys.exit(f"Claude's file still doesn't parse: {e}\n\n{text}")
            print(f"  (fixing: {e})")
            messages += [
                {"role": "assistant", "content": text},
                {"role": "user", "content": f"That file failed the parser: {e}\nSend the corrected complete file, nothing else."},
            ]
            text = ask(client, messages)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("title", help="song title, e.g. \"Ode to Joy\"")
    ap.add_argument("-o", "--out", help="where to write it (default songs/<title>.txt)")
    ap.add_argument("--bpm", type=int, help="ask for this tempo")
    ap.add_argument("--position", help="ask for this five-finger position, e.g. G4")
    ap.add_argument("--bars", type=int, help="ask for about this many bars")
    ap.add_argument("--print", action="store_true", help="print the song instead of writing a file")
    args = ap.parse_args()

    if anthropic is None:
        sys.exit("The anthropic package is not installed. Run: pip install anthropic")
    if not os.environ.get("ANTHROPIC_API_KEY") and not os.environ.get("ANTHROPIC_AUTH_TOKEN"):
        print("note: ANTHROPIC_API_KEY is not set; this only works if you are logged in another way.")

    print(f"Asking {MODEL} to arrange \"{args.title}\"...")
    try:
        text, song = compose(args.title, args.bpm, args.position, args.bars)
    except anthropic.AuthenticationError:
        sys.exit("The API key was rejected. Set ANTHROPIC_API_KEY and try again.")
    except anthropic.RateLimitError:
        sys.exit("Rate limited. Wait a moment and try again.")
    except anthropic.APIStatusError as e:
        sys.exit(f"API error {e.status_code}: {e.message}")
    except anthropic.APIConnectionError:
        sys.exit("Can't reach the API. Check the network.")

    steps, warnings = songfile.schedule(song)
    print()
    print(text.rstrip())
    print()
    print(f"{song.title}: {len(steps)} notes, {song.seconds():.0f} s at {song.bpm:g} BPM, "
          f"position {song.position} ({', '.join(songfile.semitone_name(k) for k in sorted(song.fingering))})")
    for w in warnings:
        print(f"warning: {w}")
    if args.print:
        return
    out = args.out or os.path.join(SONG_DIR, slug(args.title) + ".txt")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(text.rstrip() + "\n")
    print(f"\nwrote {out}\nplay it:  python play.py {out} --arm")


if __name__ == "__main__":
    main()
