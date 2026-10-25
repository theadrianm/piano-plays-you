// Piano Plays You - glove firmware
// Board: ESP32 Dev Module (ESP-WROOM-32). No extra libraries: servo PWM is done
// straight on the LEDC peripheral, settings use Preferences from the core.
//
// Five MG90S servos sit on the badge reels and work as finger locks:
//   LOCKED   = the reel can't pay out, so the finger can't curl and press a key
//   RELEASED = the finger is free to move
// A TENS/EMS unit on the forearm curls the fingers. Two relays gate its output.
// To play a note we release the servo(s) for that finger, close the relays for
// a short pulse, open them, and lock the servo(s) again:
//
//   P 3 300  ->  release middle --lead--> relays ON --300 ms--> relays OFF --relax--> lock middle
//
// Every finger that is not part of the note stays locked, so only the chosen
// finger moves. Nothing blocks: notes carry millis() timestamps and are advanced
// from loop(), so chords and quick passages overlap correctly and X stops
// everything within one pass of loop().
//
// Serial, 115200 baud, one command per line (Serial Monitor line ending: Newline).
// Fingers are 1..5 = thumb..pinky (piano fingering) or the letters t i m r p.
// Several fingers in one token make a chord: "13", "tm". "*" means all five.
//
//   P <fingers> [pressMs]        play a note (default press length: see T)
//   E [pressMs]                  EMS pulse without touching the servos
//   R <fingers>                  release servo(s) and leave them released
//   L <fingers>                  lock servo(s) now (cancels their notes)
//   S <finger> <angle>           move one servo to an exact angle (calibration)
//                                angles run -15..195: a little past both ends, for
//                                a horn that sits one spline tooth off
//   C                            show lock/release angles
//   C <finger> <lock> <release>  set them (saved to flash)
//   T                            show timing
//   T <lead> <relax> <gap> [press]   set timing in ms (saved to flash)
//   M <mask>                     relays an EMS pulse closes: 1, 2 or 3 = both (saved)
//   A 0|1                        disarm / arm the relays. Boots DISARMED.
//   K <relay> [ms]               close relay 1 or 2 by itself for a moment (needs arm)
//   D                            demo: five-finger scale up and back
//   V 0|1                        event log off / on
//   F                            forget saved settings, back to the defaults below
//   X                            STOP: relays open, every servo locked, notes cleared
//   ?                            status
//
// Every command answers with one line starting with "ok" or "err". Lines that
// start with "#" are information, lines that start with "!" are safety events.
//
// Safety, in order of importance:
//   * The relays are driven OFF before anything else in setup() and stay off
//     until "A 1". "A 0" and "X" open them immediately.
//   * No relay stays closed longer than MAX_RELAY_ON_MS, whatever was asked.
//     Then everything stops and the relays rest for CUTOFF_REST_MS.
//   * A rest of gapMs is forced between pulses; one pulse is capped at MAX_PRESS_MS.
//   * The TENS intensity knob is the real ceiling: the glove can only pass
//     through what a person set on the dial, never more.

#include <Preferences.h>

// ------------------------------------------------------------------ pins ----
const int   NUM_FINGERS = 5;                                     // index 0..4 = thumb..pinky
const int   SERVO_PINS[NUM_FINGERS]   = {23, 22, 21, 19, 18};
const char* FINGER_NAMES[NUM_FINGERS] = {"thumb", "index", "middle", "ring", "pinky"};
const int   NUM_RELAYS = 2;
const int   RELAY_PINS[NUM_RELAYS] = {25, 26};                   // IN1, IN2 on the relay board
const bool  RELAY_ACTIVE_LOW = true;                             // SRD-05VDC opto boards: IN low = closed
const int   LED_PIN = 2;                                         // built-in LED, on while a relay is closed

// Servo PWM on LEDC: 50 Hz, 16 bits = 0.3 us steps. 0 and 180 degrees are the
// usual 500 / 2400 us; angles may go a little past both ends (the ESP32Servo
// library would refuse that), but never past the pulse floor and ceiling,
// which is about where these servos hit their end stops.
const int SERVO_PWM_HZ   = 50;
const int SERVO_PWM_BITS = 16;
const int MIN_PULSE_US   = 500;    // 0 degrees  (SG90 / MG90S)
const int MAX_PULSE_US   = 2400;   // 180 degrees
const int PULSE_FLOOR_US = 350;
const int PULSE_CEIL_US  = 2600;
const int MIN_ANGLE = -15;         // what S and C accept
const int MAX_ANGLE = 195;
const int NO_ANGLE  = -1000;       // "no manual angle" marker (any real angle is legal, 0 and -1 included)

// ---------------------------------------------------------- hard limits ----
const unsigned long MIN_PRESS_MS    = 30;
const unsigned long MAX_PRESS_MS    = 1500;   // longest pulse a single command can ask for
const unsigned long MAX_RELAY_ON_MS = 2000;   // longest any relay may stay closed, full stop
const unsigned long CUTOFF_REST_MS  = 500;    // forced rest after a cutoff
const unsigned long DEMO_PRESS_MS   = 350;
const unsigned long DEMO_STEP_MS    = 700;
const bool          ARMED_AT_BOOT   = false;

// ------------------------------------------ settings: defaults, saved in flash ----
const int           DEFAULT_LOCK_ANGLE    = 0;    // finger can't curl
const int           DEFAULT_RELEASE_ANGLE = 90;   // finger free to press
const unsigned long DEFAULT_LEAD_MS  = 150;       // servo released -> relays close (servo travel time)
const unsigned long DEFAULT_RELAX_MS = 50;        // relays open -> servo locked again
const unsigned long DEFAULT_GAP_MS   = 150;       // minimum relays-open time between pulses (finger lift time)
const unsigned long DEFAULT_PRESS_MS = 300;       // pulse length when P / E give none
const uint8_t       DEFAULT_EMS_MASK = 0b11;      // relays closed by an EMS pulse (bit0 = relay 1, bit1 = relay 2)
const uint8_t       DEFAULT_DISABLED = 0b00001;   // fingers left alone entirely (bit0 thumb .. bit4 pinky): thumb off for now

int           lockAngle[NUM_FINGERS];
int           releaseAngle[NUM_FINGERS];
unsigned long leadMs, relaxMs, gapMs, pressMs;
uint8_t       emsMask;
uint8_t       disabledMask;              // servo never driven, dropped from every command (N)

// ----------------------------------------------------------------- state ----
struct Note {
  bool          active;
  uint8_t       fingers;                 // bit i = finger i; 0 = EMS-only pulse
  unsigned long tRelease, tOn, tOff, tLock;
};
const int MAX_NOTES = 32;
Note notes[MAX_NOTES];

Preferences prefs;

uint8_t       manualReleased = 0;        // R: fingers held released
int           manualAngle[NUM_FINGERS];  // S: exact angle, NO_ANGLE = none
bool          relayPulse[NUM_RELAYS];    // K: relay closed by hand until relayPulseEnd
unsigned long relayPulseEnd[NUM_RELAYS];
bool          armed = ARMED_AT_BOOT;
bool          verbose = true;

bool          emsOn = false;             // the logical EMS pulse
unsigned long emsOnSince = 0, emsOffSince = 0;
bool          relayOn[NUM_RELAYS];       // what the relay pins are actually doing
bool          anyRelayOn = false;
unsigned long relayOnSince = 0;
bool          resting = false;           // in the forced rest after a cutoff
unsigned long restUntil = 0;
int           servoTarget[NUM_FINGERS];  // last angle written, NO_ANGLE = none
bool          servoAttached[NUM_FINGERS];

void applyDisabled();

char          lineBuf[96];
size_t        lineLen = 0;
bool          lineOverflow = false;
bool          sawNewline = false;        // once a sender uses line endings, never guess them
unsigned long lastRxMs = 0;

// --------------------------------------------------------------- helpers ----
static inline bool after(unsigned long now, unsigned long t) {  // overflow-safe "now >= t"
  return (long)(now - t) >= 0;
}

static long clampLong(long v, long lo, long hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void ok(const char* s)  { Serial.print("ok ");  Serial.println(s); }
void err(const char* s) { Serial.print("err "); Serial.println(s); }

void relayWrite(int r, bool on) {
  digitalWrite(RELAY_PINS[r], (on != RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

int angleToPulseUs(int angle) {
  long us = MIN_PULSE_US + (long)angle * (MAX_PULSE_US - MIN_PULSE_US) / 180;
  return clampLong(us, PULSE_FLOOR_US, PULSE_CEIL_US);
}

bool servoAttach(int i) {
  return ledcAttach(SERVO_PINS[i], SERVO_PWM_HZ, SERVO_PWM_BITS);
}

void servoWrite(int i, int angle) {
  if (!servoAttached[i]) return;         // disabled finger: never driven
  const uint32_t maxDuty = (1UL << SERVO_PWM_BITS) - 1;
  const uint32_t periodUs = 1000000UL / SERVO_PWM_HZ;
  ledcWrite(SERVO_PINS[i], (uint32_t)((uint64_t)angleToPulseUs(angle) * maxDuty / periodUs));
}

const char* fingerStr(uint8_t mask) {
  static char buf[NUM_FINGERS + 2];
  int n = 0;
  for (int i = 0; i < NUM_FINGERS; i++) {
    if (mask & (1 << i)) buf[n++] = '1' + i;
  }
  if (n == 0) buf[n++] = '-';
  buf[n] = 0;
  return buf;
}

// "13", "tm", "*" -> bitmask. False on anything else or on an empty token.
bool parseFingers(const char* s, uint8_t& mask) {
  mask = 0;
  if (s == nullptr || *s == 0) return false;
  if (strcmp(s, "*") == 0 || strcasecmp(s, "all") == 0) {
    mask = (1 << NUM_FINGERS) - 1;
    return true;
  }
  for (; *s; s++) {
    int f;
    switch (tolower((unsigned char)*s)) {
      case '1': case 't': f = 0; break;
      case '2': case 'i': f = 1; break;
      case '3': case 'm': f = 2; break;
      case '4': case 'r': f = 3; break;
      case '5': case 'p': f = 4; break;
      default: return false;
    }
    mask |= 1 << f;
  }
  return true;
}

bool parseFinger(const char* s, int& f) {   // exactly one finger
  uint8_t mask;
  if (!parseFingers(s, mask)) return false;
  for (int i = 0; i < NUM_FINGERS; i++) {
    if (mask == (1 << i)) { f = i; return true; }
  }
  return false;
}

bool parseNum(const char* s, long& out, long lo, long hi) {
  if (s == nullptr || *s == 0) return false;
  char* end;
  long v = strtol(s, &end, 10);
  if (*end != 0 || v < lo || v > hi) return false;
  out = v;
  return true;
}

// -------------------------------------------------------------- settings ----
void setDefaults() {
  for (int i = 0; i < NUM_FINGERS; i++) {
    lockAngle[i]    = DEFAULT_LOCK_ANGLE;
    releaseAngle[i] = DEFAULT_RELEASE_ANGLE;
  }
  leadMs  = DEFAULT_LEAD_MS;
  relaxMs = DEFAULT_RELAX_MS;
  gapMs   = DEFAULT_GAP_MS;
  pressMs = DEFAULT_PRESS_MS;
  emsMask = DEFAULT_EMS_MASK;
  disabledMask = DEFAULT_DISABLED;
}

void loadSettings() {
  if (!prefs.begin("glove", false)) return;   // flash trouble: keep the defaults
  char key[8];
  for (int i = 0; i < NUM_FINGERS; i++) {
    snprintf(key, sizeof key, "lock%d", i);
    lockAngle[i] = clampLong(prefs.getInt(key, lockAngle[i]), MIN_ANGLE, MAX_ANGLE);
    snprintf(key, sizeof key, "rel%d", i);
    releaseAngle[i] = clampLong(prefs.getInt(key, releaseAngle[i]), MIN_ANGLE, MAX_ANGLE);
  }
  leadMs  = clampLong(prefs.getUInt("lead",  leadMs),  0, 1000);
  relaxMs = clampLong(prefs.getUInt("relax", relaxMs), 0, 2000);
  gapMs   = clampLong(prefs.getUInt("gap",   gapMs),   0, 2000);
  pressMs = clampLong(prefs.getUInt("press", pressMs), MIN_PRESS_MS, MAX_PRESS_MS);
  emsMask = clampLong(prefs.getUInt("mask",  emsMask), 1, 3);
  disabledMask = clampLong(prefs.getUInt("off", disabledMask), 0, (1 << NUM_FINGERS) - 1);
  prefs.end();
}

void saveSettings() {
  if (!prefs.begin("glove", false)) return;
  char key[8];
  for (int i = 0; i < NUM_FINGERS; i++) {
    snprintf(key, sizeof key, "lock%d", i);
    prefs.putInt(key, lockAngle[i]);
    snprintf(key, sizeof key, "rel%d", i);
    prefs.putInt(key, releaseAngle[i]);
  }
  prefs.putUInt("lead",  leadMs);
  prefs.putUInt("relax", relaxMs);
  prefs.putUInt("gap",   gapMs);
  prefs.putUInt("press", pressMs);
  prefs.putUInt("mask",  emsMask);
  prefs.putUInt("off",   disabledMask);
  prefs.end();
}

void forgetSettings() {
  if (prefs.begin("glove", false)) {
    prefs.clear();
    prefs.end();
  }
  setDefaults();
  for (int i = 0; i < NUM_FINGERS; i++) servoTarget[i] = NO_ANGLE;   // re-send the angles
  applyDisabled();
}

// ----------------------------------------------------------------- notes ----
bool scheduleNote(uint8_t fingers, unsigned long startAt, unsigned long press) {
  for (Note& n : notes) {
    if (n.active) continue;
    n.active   = true;
    n.fingers  = fingers;
    n.tRelease = startAt;
    n.tOn      = startAt + leadMs;
    n.tOff     = n.tOn + press;
    n.tLock    = n.tOff + relaxMs;
    return true;
  }
  return false;
}

void clearNotes() {
  for (Note& n : notes) n.active = false;
  for (int r = 0; r < NUM_RELAYS; r++) relayPulse[r] = false;
}

void clearManualAngles(uint8_t mask) {
  for (int i = 0; i < NUM_FINGERS; i++) {
    if (mask & (1 << i)) manualAngle[i] = NO_ANGLE;
  }
}

// Take these fingers out of every note and out of the held-released set.
void cancelFingers(uint8_t mask) {
  manualReleased &= ~mask;
  for (Note& n : notes) {
    if (!n.active || !(n.fingers & mask)) continue;
    n.fingers &= ~mask;
    if (n.fingers == 0) n.active = false;
  }
}

// Make the servos match disabledMask: a disabled finger's pin stops pulsing
// (the servo goes limp and stays wherever it is) and it drops out of every
// note; a re-enabled one is attached again and goes to its lock angle.
void applyDisabled() {
  for (int i = 0; i < NUM_FINGERS; i++) {
    const bool off = (disabledMask >> i) & 1;
    if (off && servoAttached[i]) {
      ledcDetach(SERVO_PINS[i]);
      pinMode(SERVO_PINS[i], INPUT);
      servoAttached[i] = false;
    } else if (!off && !servoAttached[i]) {
      servoAttached[i] = servoAttach(i);
      if (!servoAttached[i]) {
        Serial.print("! servo pwm failed on GPIO ");
        Serial.println(SERVO_PINS[i]);
      }
    }
    if (off || !servoAttached[i]) servoTarget[i] = NO_ANGLE;   // tick() re-sends when it comes back
  }
  cancelFingers(disabledMask);
  clearManualAngles(disabledMask);
}

void openRelays(unsigned long now) {
  for (int r = 0; r < NUM_RELAYS; r++) {
    relayPulse[r] = false;
    relayOn[r] = false;
    relayWrite(r, false);
  }
  if (emsOn) {
    emsOn = false;
    emsOffSince = now;
  }
  anyRelayOn = false;
  digitalWrite(LED_PIN, LOW);
}

void stopAll() {
  clearNotes();
  manualReleased = 0;
  for (int i = 0; i < NUM_FINGERS; i++) manualAngle[i] = NO_ANGLE;
  openRelays(millis());
  for (int i = 0; i < NUM_FINGERS; i++) {
    servoTarget[i] = lockAngle[i];
    servoWrite(i, lockAngle[i]);
  }
}

void cutoff(unsigned long now) {
  stopAll();
  resting = true;
  restUntil = now + CUTOFF_REST_MS;
  Serial.print("! cutoff: a relay was closed for ");
  Serial.print(MAX_RELAY_ON_MS);
  Serial.println(" ms, everything stopped");
}

// Advance time: retire finished notes, work out what every output should be
// doing right now, and write only the outputs that changed.
void tick() {
  const unsigned long now = millis();

  for (Note& n : notes) {
    if (n.active && after(now, n.tLock)) n.active = false;
  }

  uint8_t released = manualReleased;
  uint8_t pressing = 0;
  bool emsWanted = false;
  for (const Note& n : notes) {
    if (!n.active) continue;
    if (after(now, n.tRelease)) released |= n.fingers;
    if (after(now, n.tOn) && !after(now, n.tOff)) {
      emsWanted = true;
      pressing |= n.fingers;
    }
  }

  if (resting && after(now, restUntil)) resting = false;

  // The logical EMS pulse. Turning on needs: armed, not in a cutoff rest, and
  // the relays open for at least gapMs so the last finger had time to lift.
  if (emsOn && (!emsWanted || !armed)) {
    emsOn = false;
    emsOffSince = now;
    if (verbose) {
      Serial.print("# ems off after ");
      Serial.print(now - emsOnSince);
      Serial.println(" ms");
    }
  } else if (!emsOn && emsWanted && armed) {
    unsigned long earliest = emsOffSince + gapMs;
    if (resting && after(restUntil, earliest)) earliest = restUntil;
    if (after(now, earliest)) {
      emsOn = true;
      emsOnSince = now;
      if (verbose) {
        Serial.print("# ems on ");
        Serial.println(fingerStr(pressing));
      }
    } else {
      // Too soon. Push the waiting notes back so they still play, just late,
      // rather than losing them.
      for (Note& n : notes) {
        if (!n.active || !after(now, n.tOn) || after(now, n.tOff)) continue;
        const unsigned long press = n.tOff - n.tOn;
        n.tOn = earliest;
        n.tOff = earliest + press;
        n.tLock = n.tOff + relaxMs;
      }
    }
  }

  // Relay pins: the EMS pulse through the mask, or a K pulse. Never when
  // disarmed, never during the rest after a cutoff.
  const bool mayClose = armed && !resting;
  bool anyOn = false;
  for (int r = 0; r < NUM_RELAYS; r++) {
    if (relayPulse[r] && after(now, relayPulseEnd[r])) relayPulse[r] = false;
    bool on = mayClose && ((emsOn && ((emsMask >> r) & 1)) || relayPulse[r]);
    if (on != relayOn[r]) {
      relayOn[r] = on;
      relayWrite(r, on);
    }
    anyOn = anyOn || on;
  }
  if (anyOn && !anyRelayOn) relayOnSince = now;
  anyRelayOn = anyOn;
  digitalWrite(LED_PIN, anyOn ? HIGH : LOW);
  if (anyOn && now - relayOnSince >= MAX_RELAY_ON_MS) {
    cutoff(now);
    return;
  }

  // Servos.
  for (int i = 0; i < NUM_FINGERS; i++) {
    int angle;
    if (manualAngle[i] != NO_ANGLE)   angle = manualAngle[i];
    else if ((released >> i) & 1)     angle = releaseAngle[i];
    else                              angle = lockAngle[i];
    if (angle != servoTarget[i]) {
      servoTarget[i] = angle;
      servoWrite(i, angle);
    }
  }
}

// ---------------------------------------------------------------- status ----
void printAngles() {
  Serial.print("# angles lock/release:");
  for (int i = 0; i < NUM_FINGERS; i++) {
    Serial.print(' ');
    Serial.print(FINGER_NAMES[i]);
    Serial.print(' ');
    Serial.print(lockAngle[i]);
    Serial.print('/');
    Serial.print(releaseAngle[i]);
  }
  Serial.println();
}

void printTiming() {
  Serial.print("# timing ms: lead ");
  Serial.print(leadMs);
  Serial.print(" relax ");
  Serial.print(relaxMs);
  Serial.print(" gap ");
  Serial.print(gapMs);
  Serial.print(" press ");
  Serial.print(pressMs);
  Serial.print(" (press ");
  Serial.print(MIN_PRESS_MS);
  Serial.print("..");
  Serial.print(MAX_PRESS_MS);
  Serial.print(", cutoff ");
  Serial.print(MAX_RELAY_ON_MS);
  Serial.println(")");
}

void printStatus() {
  Serial.println("# Piano Plays You glove v1.0");
  Serial.print("# fingers:");
  for (int i = 0; i < NUM_FINGERS; i++) {
    Serial.print(' ');
    Serial.print(i + 1);
    Serial.print('=');
    Serial.print(FINGER_NAMES[i]);
    Serial.print("(GPIO");
    Serial.print(SERVO_PINS[i]);
    Serial.print(')');
  }
  Serial.print(" | relays: 1=GPIO");
  Serial.print(RELAY_PINS[0]);
  Serial.print(" 2=GPIO");
  Serial.println(RELAY_PINS[1]);
  Serial.print("# ems: ");
  Serial.print(armed ? "ARMED" : "DISARMED (send A 1 to arm)");
  if (resting) Serial.print(" | RESTING after a cutoff");
  Serial.print(" | mask ");
  Serial.print(emsMask);
  Serial.print(" | relay1 ");
  Serial.print(relayOn[0] ? "CLOSED" : "open");
  Serial.print(" relay2 ");
  Serial.println(relayOn[1] ? "CLOSED" : "open");
  printTiming();
  printAngles();
  int inFlight = 0;
  for (const Note& n : notes) {
    if (n.active) inFlight++;
  }
  Serial.print("# disabled: ");
  if (disabledMask == 0) {
    Serial.print("none");
  } else {
    for (int i = 0; i < NUM_FINGERS; i++) {
      if ((disabledMask >> i) & 1) {
        Serial.print(FINGER_NAMES[i]);
        Serial.print(' ');
      }
    }
    Serial.print("(N 0 brings them back)");
  }
  Serial.print(" | held released: ");
  Serial.print(fingerStr(manualReleased));
  Serial.print(" | notes in flight: ");
  Serial.print(inFlight);
  Serial.print(" | log ");
  Serial.println(verbose ? "on" : "off");
  Serial.println("# P fingers [ms] | E [ms] | R f | L f | S f angle | C [f lock rel] | T [lead relax gap press] | "
                 "M mask | N fingers|0 | A 0|1 | K relay [ms] | D | V 0|1 | F | X | ?");
}

// -------------------------------------------------------------- commands ----
void handleLine(char* line) {
  char* tok[6];
  int ntok = 0;
  char* save = nullptr;
  for (char* t = strtok_r(line, " \t", &save); t != nullptr && ntok < 6; t = strtok_r(nullptr, " \t", &save)) {
    tok[ntok++] = t;
  }
  if (ntok == 0) {
    err("empty line");
    return;
  }
  if (strlen(tok[0]) != 1) {
    err("unknown command, send ? for help");
    return;
  }

  const char cmd = toupper((unsigned char)tok[0][0]);
  const unsigned long now = millis();
  uint8_t mask = 0;
  int f = 0;
  long a = 0, b = 0, c = 0, d = 0;

  switch (cmd) {
    case 'P':
      if (ntok < 2 || !parseFingers(tok[1], mask)) { err("P <fingers> [pressMs]   e.g. P 3 300 or P 13 400"); return; }
      a = pressMs;
      if (ntok >= 3 && !parseNum(tok[2], a, MIN_PRESS_MS, MAX_PRESS_MS)) { err("press must be 30..1500 ms"); return; }
      mask &= ~disabledMask;
      if (mask == 0) { ok("P skipped: finger disabled"); return; }
      if (!scheduleNote(mask, now, a)) { err("too many notes in flight"); return; }
      clearManualAngles(mask);
      Serial.print("ok P ");
      Serial.print(fingerStr(mask));
      Serial.print(' ');
      Serial.print(a);
      Serial.println(armed ? "" : " (disarmed: servos only)");
      return;

    case 'E':
      a = pressMs;
      if (ntok >= 2 && !parseNum(tok[1], a, MIN_PRESS_MS, MAX_PRESS_MS)) { err("E [pressMs]   30..1500"); return; }
      if (!armed) { err("disarmed, send A 1 first"); return; }
      if (!scheduleNote(0, now, a)) { err("too many notes in flight"); return; }
      Serial.print("ok E ");
      Serial.println(a);
      return;

    case 'R':
      if (ntok < 2 || !parseFingers(tok[1], mask)) { err("R <fingers>"); return; }
      mask &= ~disabledMask;
      if (mask == 0) { ok("R skipped: finger disabled"); return; }
      clearManualAngles(mask);
      manualReleased |= mask;
      Serial.print("ok R ");
      Serial.println(fingerStr(mask));
      return;

    case 'L':
      if (ntok < 2 || !parseFingers(tok[1], mask)) { err("L <fingers>   (* = all)"); return; }
      clearManualAngles(mask);
      cancelFingers(mask);
      Serial.print("ok L ");
      Serial.println(fingerStr(mask));
      return;

    case 'S':
      if (ntok < 3 || !parseFinger(tok[1], f) || !parseNum(tok[2], a, MIN_ANGLE, MAX_ANGLE)) { err("S <finger> <angle -15..195>"); return; }
      if ((disabledMask >> f) & 1) { ok("S skipped: finger disabled"); return; }
      cancelFingers(1 << f);
      manualAngle[f] = a;
      Serial.print("ok S ");
      Serial.print(f + 1);
      Serial.print(' ');
      Serial.println(a);
      return;

    case 'C':
      if (ntok == 1) { printAngles(); ok("C"); return; }
      if (ntok < 4 || !parseFinger(tok[1], f) || !parseNum(tok[2], a, MIN_ANGLE, MAX_ANGLE) || !parseNum(tok[3], b, MIN_ANGLE, MAX_ANGLE)) {
        err("C <finger> <lockAngle> <releaseAngle>   angles -15..195");
        return;
      }
      lockAngle[f] = a;
      releaseAngle[f] = b;
      servoTarget[f] = NO_ANGLE;
      saveSettings();
      Serial.print("ok C ");
      Serial.print(FINGER_NAMES[f]);
      Serial.print(" lock ");
      Serial.print(a);
      Serial.print(" release ");
      Serial.print(b);
      Serial.println(" (saved)");
      return;

    case 'T':
      if (ntok == 1) { printTiming(); ok("T"); return; }
      d = pressMs;
      if (ntok < 4 || !parseNum(tok[1], a, 0, 1000) || !parseNum(tok[2], b, 0, 2000) || !parseNum(tok[3], c, 0, 2000)
          || (ntok >= 5 && !parseNum(tok[4], d, MIN_PRESS_MS, MAX_PRESS_MS))) {
        err("T <lead 0..1000> <relax 0..2000> <gap 0..2000> [press 30..1500]");
        return;
      }
      leadMs = a;
      relaxMs = b;
      gapMs = c;
      pressMs = d;
      saveSettings();
      printTiming();
      ok("T (saved)");
      return;

    case 'M':
      if (ntok < 2 || !parseNum(tok[1], a, 1, 3)) { err("M <mask 1|2|3>   1 = relay 1, 2 = relay 2, 3 = both"); return; }
      emsMask = a;
      saveSettings();
      Serial.print("ok M ");
      Serial.print(a);
      Serial.println(" (saved)");
      return;

    case 'N':   // fingers to leave alone: "N 1" = thumb off, "N 15" = thumb and pinky off, "N 0" = all on
      if (ntok < 2) { err("N <fingers to disable, or 0 for none>"); return; }
      if (strcmp(tok[1], "0") == 0 || strcmp(tok[1], "-") == 0 || strcasecmp(tok[1], "none") == 0) {
        mask = 0;
      } else if (!parseFingers(tok[1], mask)) {
        err("N <fingers to disable, or 0 for none>");
        return;
      }
      disabledMask = mask;
      saveSettings();
      applyDisabled();
      Serial.print("ok N disabled: ");
      Serial.print(fingerStr(disabledMask));
      Serial.println(" (saved)");
      return;

    case 'A':
      if (ntok < 2 || !parseNum(tok[1], a, 0, 1)) { err("A 0|1"); return; }
      clearNotes();   // arming never starts anything by itself, disarming never leaves anything pending
      armed = (a == 1);
      if (!armed) openRelays(now);
      ok(armed ? "A 1 ARMED: relays live" : "A 0 disarmed: relays open");
      return;

    case 'K':
      a = 1000;
      if (ntok < 2 || !parseNum(tok[1], b, 1, NUM_RELAYS) || (ntok >= 3 && !parseNum(tok[2], a, MIN_PRESS_MS, MAX_PRESS_MS))) {
        err("K <relay 1|2> [ms 30..1500]");
        return;
      }
      if (!armed) { err("disarmed, send A 1 first"); return; }
      if (resting) { err("resting after a cutoff, try again in a moment"); return; }
      relayPulse[b - 1] = true;
      relayPulseEnd[b - 1] = now + a;
      Serial.print("ok K relay ");
      Serial.print(b);
      Serial.print(" closed for ");
      Serial.print(a);
      Serial.println(" ms");
      return;

    case 'D': {
      clearNotes();
      manualReleased = 0;
      clearManualAngles((1 << NUM_FINGERS) - 1);
      const int order[] = {0, 1, 2, 3, 4, 3, 2, 1, 0};
      unsigned long t = now + 300;
      for (unsigned k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
        if ((disabledMask >> order[k]) & 1) continue;
        scheduleNote(1 << order[k], t, DEMO_PRESS_MS);
        t += DEMO_STEP_MS;
      }
      ok(armed ? "D demo: scale up and back" : "D demo: scale up and back (disarmed: servos only)");
      return;
    }

    case 'V':
      if (ntok < 2 || !parseNum(tok[1], a, 0, 1)) { err("V 0|1"); return; }
      verbose = (a == 1);
      ok(verbose ? "V 1 log on" : "V 0 log off");
      return;

    case 'F':
      forgetSettings();
      printTiming();
      printAngles();
      ok("F defaults restored");
      return;

    case 'X':
      stopAll();
      ok("X stopped: relays open, all servos locked");
      return;

    case '?':
      printStatus();
      ok("?");
      return;

    default:
      err("unknown command, send ? for help");
      return;
  }
}

// At most one command per pass, and a bounded number of bytes, so tick() keeps
// running however fast a host talks to us (replies block on the UART FIFO).
void pollSerial() {
  for (int budget = 0; budget < 64 && Serial.available() > 0; budget++) {
    char ch = (char)Serial.read();
    lastRxMs = millis();
    if (ch == '\n' || ch == '\r') {
      sawNewline = true;
      lineOverflow = false;
      if (lineLen > 0) {
        lineBuf[lineLen] = 0;
        lineLen = 0;
        handleLine(lineBuf);
        return;
      }
      continue;
    }
    if (lineOverflow) continue;
    if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = ch;
    } else {
      lineOverflow = true;
      lineLen = 0;
      err("line too long");
      return;
    }
  }
  // Serial Monitor set to "No line ending": 200 ms of silence ends the line.
  // A sender that has used a line ending once never gets second-guessed.
  if (!sawNewline && millis() - lastRxMs > 200) {
    lineOverflow = false;
    if (lineLen > 0) {
      lineBuf[lineLen] = 0;
      lineLen = 0;
      handleLine(lineBuf);
    }
  }
}

// Everything back to the state a fresh power-up has. Static initialisation
// does this already on a real reset; doing it here too makes setup() safe to
// reason about on its own.
void resetState() {
  for (Note& n : notes) n.active = false;
  manualReleased = 0;
  for (int i = 0; i < NUM_FINGERS; i++) {
    manualAngle[i] = NO_ANGLE;
    servoTarget[i] = NO_ANGLE;
    servoAttached[i] = false;
  }
  for (int r = 0; r < NUM_RELAYS; r++) {
    relayPulse[r] = false;
    relayOn[r] = false;
  }
  armed = ARMED_AT_BOOT;
  verbose = true;
  emsOn = false;
  anyRelayOn = false;
  resting = false;
  restUntil = 0;
  lineLen = 0;
  lineOverflow = false;
  sawNewline = false;
}

// ------------------------------------------------------------------ main ----
void setup() {
  // Relays first. Pull the inputs to their OFF level before they become
  // outputs so the TENS side can't see even a blip while booting (on core 3.x
  // digitalWrite() does nothing on a pin that has had no pinMode() yet).
  for (int r = 0; r < NUM_RELAYS; r++) {
    pinMode(RELAY_PINS[r], RELAY_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
    relayWrite(r, false);
    pinMode(RELAY_PINS[r], OUTPUT);
    relayWrite(r, false);
  }
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(115200);

  resetState();
  setDefaults();
  loadSettings();

  for (int i = 0; i < NUM_FINGERS; i++) {
    if ((disabledMask >> i) & 1) continue;      // disabled finger: pin stays idle, servo stays limp
    servoAttached[i] = servoAttach(i);
    if (!servoAttached[i]) {
      Serial.print("! servo pwm failed on GPIO ");
      Serial.println(SERVO_PINS[i]);
    }
    servoWrite(i, lockAngle[i]);
    servoTarget[i] = lockAngle[i];
    delay(200);   // stagger so they don't all jerk at once
  }
  emsOffSince = millis();

  Serial.println();
  printStatus();
  Serial.println("# ready");
}

void loop() {
  pollSerial();
  tick();
}
