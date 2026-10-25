# Piano Plays You

A haptic glove that plays the piano with your hand. Five servos on badge reels lock the fingers that shouldn't move, and a TENS/EMS unit on the forearm curls the one that should. The judge picks the song, Claude arranges it, the glove plays it.

```
laptop (host/play.py)  --USB serial-->  ESP32 (firmware/glove)  --> 5 servos (finger locks)
                                                                 --> 2 relays (gate the TENS output)
```

## Hardware

| Thing | Where |
|---|---|
| ESP32 Dev Module (ESP-WROOM-32) | USB to the laptop |
| Servo thumb / index / middle / ring / pinky | GPIO 23 / 22 / 21 / 19 / 18 |
| Relay 1 / relay 2 (IN1 / IN2 on the 8-relay board) | GPIO 25 / 26 |
| Relay board GND / VCC | GND / 5 V (JD-VCC jumper on) |
| Built-in LED | GPIO 2, on while a relay is closed |
| Servo power | separate 5 V, 1000 µF across the rail, ground shared with the ESP32 |
| TENS 7000 | leads go through relay 1 and 2 COM + NO screw terminals |

The potentiometers are not used. The firmware only needs to know whether a key should go down, not how far the finger moved.

## Firmware (`firmware/glove`)

Open `firmware/glove/glove.ino` in the Arduino IDE, board **ESP32 Dev Module**, no extra libraries, upload, Serial Monitor at **115200** with line ending **Newline**. (`firmware/servo_test` and `firmware/pot_test` are the earlier bring-up sketches; those two do need the ESP32Servo library.)

### What a note is

Every finger idles **locked**. `P 3 300` plays the middle finger for 300 ms:

```
 release middle servo --lead 150--> relays close --300 ms--> relays open --relax 50--> lock middle servo
```

The other four fingers stay locked, so when the EMS fires, only the middle finger can curl. The next note waits until the relays have been open for `gap` (150 ms, time for the finger to lift); a note that arrives too early is pushed back rather than dropped, so a song that is too fast runs late instead of losing notes. Notes whose press windows overlap (a chord sent as two commands, a legato pair) share one pulse. Chords are one token: `P 13 400`. Everything is timestamped and advanced from `loop()`, nothing blocks, and `X` stops it all within a millisecond.

### Commands

Fingers are `1`..`5` = thumb..pinky (piano fingering) or `t i m r p`. Several in one token make a chord (`13`, `tm`), `*` is all five. Every command answers one line starting with `ok` or `err`; `#` lines are information, `!` lines are safety events.

| Command | Does |
|---|---|
| `P <fingers> [ms]` | play a note (default press length from `T`) |
| `E [ms]` | EMS pulse without touching the servos |
| `R <fingers>` / `L <fingers>` | release / lock servos and leave them there |
| `S <finger> <angle>` | move one servo to an exact angle (calibration) |
| `C` / `C <finger> <lock> <release>` | show / set the lock and release angles (saved to flash) |
| `T` / `T <lead> <relax> <gap> [press]` | show / set timing in ms (saved to flash) |
| `M <1\|2\|3>` | which relays an EMS pulse closes (3 = both, the default) |
| `A 0\|1` | disarm / arm the relays. **Boots disarmed.** Either way clears notes in flight, so arming never starts anything by itself. |
| `K <relay> [ms]` | close one relay by itself for a moment (needs arm) |
| `D` | demo: five-finger scale up and back |
| `V 0\|1` | event log off / on |
| `F` | forget saved settings |
| `X` | **stop**: relays open, every servo locked, notes cleared |
| `?` | status |

### Safety

- The relays are driven off before anything else in `setup()` and stay off until `A 1`. `A 0` and `X` open them immediately, and arming never resumes anything that was queued.
- No relay stays closed longer than **2 s**, whatever was asked. Then everything stops and the relays rest for 0.5 s (`! cutoff` on serial).
- One press is capped at 1.5 s and a 150 ms rest is forced between presses.
- One command is handled per pass of `loop()`, so the timing code keeps running however fast the laptop talks.
- Settings changes and the arm state are separate: arming never survives a reboot.
- The TENS intensity knob is the real ceiling. The glove only passes through what a person set on the dial, never more.

`firmware/sim/run.sh` compiles the sketch against stub Arduino headers and drives it with a simulated clock through the sequences above (overlaps, the cutoff, disarm mid-pulse, clock wraparound, serial edge cases). Run it after changing the firmware.

## Bring-up checklist

Do this once, in order, **with the TENS unplugged from the relays**.

1. **Upload, open the Serial Monitor.** You should see the status banner ending in `# ready`. Send `?`.
2. **Servos.** Send `D`. Each finger's servo should move in turn: thumb, index, middle, ring, pinky, and back. If the order is wrong, swap plugs, don't edit code.
3. **Calibrate the locks.** Every servo horn went on at a slightly different angle, so each finger gets its own lock and release angle. In a terminal window:
   ```
   cd host && .venv/bin/python calibrate.py
   ```
   Pick a finger with `1`–`5`, nudge the live servo with `a`/`d` (±1) or `A`/`D` (±10) until the badge reel is blocked, press `space` and nudge until it runs free, `s` saves that finger to the glove's flash, `t` saves and plays it once. `g` parks every finger at its lock angle so you can compare them side by side. The same thing by hand over serial: `S 1 40` moves the thumb to 40°, `C 1 40 130` saves lock 40 / release 130, `C` prints the table.

   Angles run from **−15 to 195**, a little past the usual 0–180, which covers a horn that went on one spline tooth off. A servo that buzzes has hit its mechanical end stop: back off a few degrees, or re-seat the horn (hold the servo at 30 with `calibrate.py move all 30`, pull the horn, press it back on in the blocked position).
4. **Relays, TENS still unplugged.** `A 1`, then `K 1 1000`: relay 1 clicks and its LED lights for one second. `K 2 1000` for relay 2. (`python host/relay_test.py --yes` does this sequence for you.) With a multimeter in continuity mode on the two screw terminals you wired: **no beep when the LED is off, beep when it is on.** If it beeps when off, the wire is on NC instead of NO. Fix that before going further. No click and no LED at all means the relay board has no 5 V: its VCC comes off the servo rail, so the battery has to be on.
5. **Pick the mask.** Both relays close for a note by default (`M 3`). If only one is in the TENS path, `M 1` or `M 2`.
6. **TENS, low.** Plug the TENS in, intensity near zero, pads on the forearm, `A 1`, `E 300`. Raise the dial until the fingers twitch. Then `P 2 300` with the glove on: only the index finger should press.
7. **Power.** Plug the servo battery in **after** uploading (power banks switch off when nothing draws current during the upload). Keep the 1000 µF capacitor on the servo rail. Avoid holding all five servos stalled at once.

## Playing songs (`host/`)

```bash
cd host
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt

python play.py --show songs/twinkle.txt        # look at the schedule, no glove needed
python play.py songs/twinkle.txt               # dry run: servos move, EMS disarmed
python play.py songs/twinkle.txt --arm         # the real thing
python play.py --repl                          # type glove commands, good for calibration
```

Close the Arduino Serial Monitor first; only one program can hold the port. `play.py` finds the USB port by itself (`--port` to pick one, `--list-ports` to see them). It sets the firmware timing to match its own schedule (`--lead`, `--relax`, `--gap`), arms only with `--arm`, waits for Enter, streams one `P` per note at the right moment, and disarms on exit or Ctrl-C (which also sends `X`).

The EMS press for a note is the note length minus the 150 ms gap the next finger needs to lift. Notes shorter than about 290 ms can't be played cleanly; `--show` warns and suggests a tempo.

### Song files

```
title: Ode to Joy
bpm: 100
position: C4        # key under the thumb; the five white keys from here are fingers 1-5
hand: right

E4 E4 F4 G4 | G4 F4 E4 D4 | C4 C4 D4 E4 | E4:1.5 D4:0.5 D4:2
```

A token is `<pitch>[:beats]`: a note inside the position (`G4`), finger numbers (`1`, or `13` for a chord), or a rest (`r`). `[C4 E4]:2` is a chord by name. `|` and `# comments` are ignored. `map: C4=1 D4=2 E4=3 F4=4 G4=5` replaces `position`/`hand` with an explicit list; a tune that needs a sixth key can map it onto a neighbour (`A4=5`, see `songs/twinkle.txt`). Five fingers, five keys: a melody wider than that has to be simplified, which is what `compose.py` asks Claude to do. See `host/songs/`.

### Judge picks a song

```bash
export ANTHROPIC_API_KEY=...
python compose.py "Happy Birthday"               # Claude arranges it for five fingers -> songs/happy-birthday.txt
python play.py songs/happy-birthday.txt --arm
```

`compose.py` asks Claude Opus 5.5 for the arrangement in the song format above, runs it through the same parser `play.py` uses, and asks for a fix once if it doesn't parse. Claude chooses the position and tempo; Claude never touches intensity, which stays on the dial.

## Troubleshooting

- **Nothing moves after upload, works after replugging the battery.** Power bank auto-shutoff. Plug the battery in after the upload, or power the ESP32 from it too so there is always a load.
- **Servos twitch or the ESP32 resets when a note plays.** Servo rail sagging. Check the 1000 µF cap and the common ground, use the bigger battery.
- **Commands do nothing in the Serial Monitor.** Line ending must be Newline (the firmware also accepts a 200 ms pause as end of line).
- **`P` moves the servo but nothing happens on the arm.** `?` says DISARMED: send `A 1`. Or `M` points at the wrong relay.
- **`! cutoff` on serial.** Something kept a relay closed for 2 s. Everything stopped; send `?`, then carry on.
- **play.py: can't open port.** The Serial Monitor is still open, or two ports are listed (pass `--port`).
