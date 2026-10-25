// Drives glove.ino on a laptop with a simulated clock and checks the things
// that matter before it goes on an arm. Run:  ./run.sh   (needs clang++ or g++)
//
// Timing model: run(n) calls loop() once per simulated millisecond, then
// advances the clock. cmd() feeds one line and runs a single millisecond, so a
// command sent "at T" is processed in the loop() call at time T.
#include "Arduino.h"
#include "ESP32Servo.h"
#include "Preferences.h"

unsigned long g_now = 0;
int g_pins[64];
int g_pinMode[64];
int g_servoAngle[64];
FakeSerial Serial;
std::map<std::string, long> g_nvs;

#include "../glove/glove.ino"

// ------------------------------------------------------------ harness ----
static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                              \
  do {                                                                \
    if (cond) { g_pass++; }                                           \
    else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
           printf(__VA_ARGS__); printf("\n"); }                       \
  } while (0)

static void feed(const char* s) { Serial.in += s; }
static std::string take() { std::string s = Serial.out; Serial.out.clear(); return s; }
static void run(unsigned long ms) {
  for (unsigned long i = 0; i < ms; i++) { loop(); g_now++; }
}
static bool closed(int r) { return g_pins[RELAY_PINS[r]] == LOW; }   // active low
static bool anyClosed() { return closed(0) || closed(1); }
static int angle(int finger) { return g_servoAngle[SERVO_PINS[finger]]; }
static int count(const std::string& s, const char* needle) {
  int n = 0;
  for (size_t p = s.find(needle); p != std::string::npos; p = s.find(needle, p + 1)) n++;
  return n;
}
static int replies(const std::string& s) {
  return count(s, "\nok ") + (s.rfind("ok ", 0) == 0) + count(s, "\nerr ") + (s.rfind("err ", 0) == 0);
}
// Send one command at the current time, run 1 ms, return the output. Every
// command must answer exactly one ok/err line.
static std::string cmd(const char* line) {
  feed(line); feed("\n"); run(1);
  std::string out = take();
  CHECK(replies(out) == 1, "'%s' should answer exactly one ok/err line, got: %s", line, out.c_str());
  return out;
}
static bool ok(const std::string& r) { return r.find("ok ") != std::string::npos && r.find("err ") == std::string::npos; }
static bool err(const std::string& r) { return r.find("err ") != std::string::npos; }

// Run until a relay closes; returns the time of the loop() that closed it, or 0.
static unsigned long waitClosed(unsigned long limit) {
  for (unsigned long n = 0; n < limit; n++) {
    if (anyClosed()) return g_now - 1;
    run(1);
  }
  return anyClosed() ? g_now - 1 : 0;
}
// Run until every relay is open; returns the time of the loop() that opened them, or 0.
static unsigned long waitOpen(unsigned long limit) {
  for (unsigned long n = 0; n < limit; n++) {
    if (!anyClosed()) return g_now - 1;
    run(1);
  }
  return anyClosed() ? 0 : g_now - 1;
}
static void quiet() {            // X, then long enough for any rest to end
  cmd("X");
  run(700);
  take();
}
static void reboot() {           // power cycle: RAM is fresh, flash (g_nvs) is kept
  memset(g_pins, -1, sizeof g_pins);
  memset(g_pinMode, -1, sizeof g_pinMode);
  memset(g_servoAngle, -1, sizeof g_servoAngle);
  Serial.in.clear(); Serial.out.clear();
  setup();
  take();
}

int main() {
  unsigned long t, on, off;
  std::string r;

  // ---- boot -------------------------------------------------------------
  g_now = 5000;
  reboot();
  CHECK(!anyClosed() && g_pins[25] == HIGH && g_pins[26] == HIGH, "relays boot open (pins high)");
  CHECK(g_pinMode[25] == OUTPUT && g_pinMode[26] == OUTPUT, "relay pins are outputs after setup");
  for (int f = 0; f < 5; f++) CHECK(angle(f) == 0, "finger %d boots locked at 0, got %d", f, angle(f));
  CHECK(g_pins[LED_PIN] == LOW, "LED off at boot");

  // ---- disarmed note: servo choreography only ----------------------------
  r = cmd("P 3 300");
  CHECK(ok(r) && r.find("disarmed") != std::string::npos, "P while disarmed is ok but says so: %s", r.c_str());
  CHECK(angle(2) == 90, "middle released at once, got %d", angle(2));
  run(600);
  CHECK(!anyClosed(), "no relay may close while disarmed");
  CHECK(take().find("# ems") == std::string::npos, "no ems log while disarmed");
  CHECK(angle(2) == 0, "middle locked again after the note, got %d", angle(2));

  // ---- armed note: lead / press / relax -----------------------------------
  CHECK(ok(cmd("A 1")), "arm");
  t = g_now;
  cmd("P 3 300");                                   // sent at t
  CHECK(angle(2) == 90, "released at t");
  on = waitClosed(1000);
  CHECK(on == t + 150, "relays close at t+150, got t+%lu", on - t);
  CHECK(closed(0) && closed(1), "mask 3 closes both relays");
  CHECK(g_pins[LED_PIN] == HIGH, "LED mirrors the relays");
  off = waitOpen(2000);
  CHECK(off == t + 450, "relays open at t+450, got t+%lu", off - t);
  CHECK(angle(2) == 90 && g_pins[LED_PIN] == LOW, "still released during relax, LED off");
  run(48);                                          // now = t+499
  CHECK(angle(2) == 90, "released until relax is over");
  run(1);                                           // loop at t+499 ran; now t+500
  CHECK(angle(2) == 90, "not yet");
  run(1);                                           // loop at t+500: tLock reached
  CHECK(angle(2) == 0, "locked at t+500, got %d", angle(2));
  r = take();
  CHECK(r.find("# ems on 3") != std::string::npos && r.find("# ems off after 300 ms") != std::string::npos, "ems log: %s", r.c_str());

  // ---- chord + default press + verbose off --------------------------------
  CHECK(ok(cmd("V 0")), "V 0");
  r = cmd("P 15");
  CHECK(r.find("ok P 15 300") != std::string::npos, "default press 300: %s", r.c_str());
  CHECK(angle(0) == 90 && angle(4) == 90 && angle(1) == 0, "thumb and pinky released, index locked");
  run(600);
  CHECK(take().empty(), "nothing logged with V 0");

  // ---- press windows that overlap share one pulse -------------------------
  t = g_now;
  cmd("P 1 300");                                   // press t+150 .. t+450
  on = waitClosed(1000);
  CHECK(on == t + 150, "first note closes at t+150");
  run(49);                                          // now t+200
  cmd("P 2 300");                                   // sent t+200: press t+350 .. t+650
  off = waitOpen(2000);
  CHECK(off == t + 650, "one continuous pulse until the second note ends, got t+%lu", off - t);
  CHECK(angle(0) == 0, "thumb re-locked at its own time (t+500) while the pulse went on");
  CHECK(angle(1) == 90, "index still released");
  run(49); run(1);
  CHECK(angle(1) == 0, "index locked at t+700");

  // ---- press windows that don't overlap: off, gap, on again ---------------
  t = g_now;
  cmd("P 1 300");                                   // press t+150 .. t+450
  run(399);
  cmd("P 2 300");                                   // sent t+400: wants t+550 .. t+850
  waitClosed(1000);
  off = waitOpen(2000);
  CHECK(off == t + 450, "pulse ends with note 1 at t+450, got t+%lu", off - t);
  on = waitClosed(2000);
  CHECK(on == t + 600, "note 2 waits for the 150 ms gap: on at t+600, got t+%lu", on - t);
  off = waitOpen(2000);
  CHECK(off == t + 900, "and keeps its full 300 ms press, got t+%lu", off - t);
  quiet();

  // ---- a bigger gap delays a short note rather than dropping it -----------
  cmd("T 150 50 400");
  t = g_now;
  cmd("P 1 300");                                   // press t+150 .. t+450
  run(449);
  cmd("P 2 100");                                   // sent t+450 as note 1 ends: wants t+600 .. t+700, gap says >= t+850
  CHECK(!anyClosed(), "note 1's pulse ended at t+450");
  on = waitClosed(2000);
  CHECK(on == t + 850, "short note pushed to t+850, got t+%lu", on - t);
  off = waitOpen(2000);
  CHECK(off == t + 950, "and still gets its 100 ms, got t+%lu", off - t);
  run(55);
  CHECK(angle(1) == 0, "index locked after the pushed note");
  cmd("T 150 50 150");
  quiet();

  // ---- press cap and argument checks --------------------------------------
  CHECK(err(cmd("P 3 1501")), "press over 1500 is refused");
  CHECK(err(cmd("P 3 29")), "press under 30 is refused");
  CHECK(err(cmd("P 6")), "finger 6 refused");
  CHECK(err(cmd("P")), "P without fingers refused");
  CHECK(err(cmd("Q")), "unknown command refused");
  CHECK(err(cmd("PX 1")), "two-letter command refused");
  CHECK(ok(cmd("p tm 200")), "lowercase command and letter fingers");
  quiet();

  // ---- cutoff: nothing keeps a relay closed past 2000 ms ------------------
  cmd("E 1500");                                    // press +150 .. +1650
  on = waitClosed(1000);
  run(1250);
  cmd("E 1500");                                    // overlaps: would run to +3151
  off = waitOpen(5000);
  CHECK(off - on == 2000, "cutoff after exactly 2000 ms closed, got %lu", off - on);
  r = take();
  CHECK(r.find("! cutoff") != std::string::npos, "cutoff announced: %s", r.c_str());
  CHECK(!anyClosed() && angle(0) == 0, "cutoff opened relays and locked servos");
  r = cmd("?");
  CHECK(r.find("RESTING") != std::string::npos, "status shows the rest: %s", r.c_str());
  cmd("E 300");                                     // during the rest: pushed back, not lost
  on = waitClosed(2000);
  CHECK(on == off + 500, "first pulse after a cutoff waits the 500 ms rest, got +%lu", on - off);
  t = waitOpen(2000);
  CHECK(t - on == 300, "and plays in full, got %lu", t - on);
  quiet();

  // ---- K pulses: single relay, timed, blocked while disarmed/resting ------
  r = cmd("K 1 1000");
  CHECK(ok(r) && closed(0) && !closed(1), "K 1 closes only relay 1 at once: %s", r.c_str());
  on = g_now - 1;
  off = waitOpen(2000);
  CHECK(off - on == 1000, "K 1 1000 lasts 1000 ms, got %lu", off - on);
  CHECK(err(cmd("K 3")), "relay 3 does not exist");
  CHECK(err(cmd("K 1 20")), "K pulse under 30 ms refused");
  cmd("K 1 1500");
  on = g_now - 1;
  run(1400);
  cmd("K 2 1500");                                  // continuous closure continues on relay 2
  off = waitOpen(5000);
  CHECK(off - on == 2000 && take().find("! cutoff") != std::string::npos, "K pulses are under the cutoff too, got %lu", off - on);
  r = cmd("K 1 300");
  CHECK(err(r) && r.find("resting") != std::string::npos, "K refused during the rest: %s", r.c_str());
  run(600);
  CHECK(!anyClosed(), "nothing closed by itself after the rest");
  CHECK(ok(cmd("K 2 100")) && closed(1) && !closed(0), "K works again after the rest");
  quiet();

  // ---- A 0 and X open relays immediately; arming never resumes anything --
  cmd("P 3 1000"); run(300);
  CHECK(anyClosed(), "pulse running");
  cmd("A 0");
  CHECK(!anyClosed() && g_pins[LED_PIN] == LOW, "A 0 opens relays at once");
  run(1);
  CHECK(angle(2) == 0, "A 0 drops the note: finger locked, got %d", angle(2));
  run(2000);
  CHECK(!anyClosed(), "stays open while disarmed");
  CHECK(err(cmd("E 300")) && err(cmd("K 1")), "E and K refused while disarmed");
  cmd("P 1 1500"); run(100);                        // scheduled while disarmed
  cmd("A 1");
  run(1800);
  CHECK(!anyClosed() && take().find("# ems") == std::string::npos, "a note scheduled while disarmed does not fire on A 1");
  cmd("D"); run(1000);                              // demo under way
  cmd("A 0"); run(500); cmd("A 1"); run(6000);
  CHECK(take().find("# ems") == std::string::npos, "A 0 then A 1 does not resume the demo");
  cmd("P 3 1000"); run(300);
  CHECK(anyClosed() && angle(2) == 90, "pulse running again");
  r = cmd("X");
  CHECK(ok(r) && !anyClosed() && angle(2) == 0, "X opens relays and locks servos at once");
  run(2000);
  CHECK(!anyClosed(), "nothing comes back after X");

  // ---- R / L / S and calibration -----------------------------------------
  cmd("R 2");
  CHECK(angle(1) == 90, "R releases");
  run(500);
  CHECK(angle(1) == 90, "R holds");
  cmd("P 2 200"); run(600);
  CHECK(angle(1) == 90, "finger held by R stays released after its note");
  cmd("L 2");
  CHECK(angle(1) == 0, "L locks at once");
  cmd("P 3 500"); run(200);
  CHECK(anyClosed() && angle(2) == 90, "note on middle running");
  cmd("L 3");
  CHECK(angle(2) == 0, "L cancels the finger's note");
  run(2);
  CHECK(!anyClosed(), "and its pulse");
  cmd("S 4 123");
  CHECK(angle(3) == 123, "S moves to an exact angle");
  run(1000);
  CHECK(angle(3) == 123, "S holds until told otherwise");
  cmd("P 4 100");
  CHECK(angle(3) == 90, "P clears the S override");
  run(500);
  CHECK(angle(3) == 0, "back to lock");
  cmd("R 2"); cmd("S 4 77");
  cmd("D");
  run(1);
  CHECK(angle(1) == 0 && angle(3) == 0, "D clears R holds and S parks, got %d %d", angle(1), angle(3));
  cmd("X");
  CHECK(err(cmd("S 4 181")) && err(cmd("S 45 10")), "S argument checks");
  r = cmd("C 3 10 120");
  CHECK(ok(r) && angle(2) == 10, "new lock angle applied at once: %s", r.c_str());
  cmd("P 3 100");
  CHECK(angle(2) == 120, "new release angle used");
  run(400);
  CHECK(angle(2) == 10, "and new lock angle after");
  CHECK(err(cmd("C 3 10")) && err(cmd("C 3 10 181")), "C argument checks");
  r = cmd("C");
  CHECK(r.find("middle 10/120") != std::string::npos, "C shows the table: %s", r.c_str());
  r = cmd("T 100 20 200 250");
  CHECK(r.find("lead 100 relax 20 gap 200 press 250") != std::string::npos, "T sets and shows: %s", r.c_str());
  CHECK(err(cmd("T 1001 20 200")) && err(cmd("T 100 20")), "T argument checks");
  CHECK(ok(cmd("M 1")), "M 1");
  cmd("P 3"); run(150);
  CHECK(closed(0) && !closed(1), "mask 1 closes relay 1 only");
  run(600);
  CHECK(err(cmd("M 0")) && err(cmd("M 4")), "M range");

  // ---- settings survive a reboot; arm state does not; F forgets ----------
  g_now += 10;
  reboot();
  CHECK(angle(2) == 10, "lock angle restored from flash after reboot, got %d", angle(2));
  r = cmd("T");
  CHECK(r.find("lead 100 relax 20 gap 200 press 250") != std::string::npos, "timing restored: %s", r.c_str());
  CHECK(!anyClosed() && err(cmd("E 300")), "boots disarmed even if it was armed before");
  cmd("P 3 100"); run(400);
  CHECK(!anyClosed(), "and a note closes nothing");
  r = cmd("F");
  CHECK(ok(r) && angle(2) == 0, "F restores defaults and re-sends the angle");
  r = cmd("T");
  CHECK(r.find("lead 150 relax 50 gap 150 press 300") != std::string::npos, "default timing back: %s", r.c_str());
  reboot();
  CHECK(angle(2) == 0, "defaults persist after F + reboot");
  cmd("A 1");

  // ---- serial edge cases --------------------------------------------------
  reboot();                                         // fresh: no line ending seen yet
  feed("?"); run(150);                              // Serial Monitor with "No line ending"
  CHECK(take().empty(), "not processed yet without a newline");
  run(60);
  CHECK(replies(take()) == 1, "idle flush processed it after 200 ms of silence");
  std::string overlong(120, 'P');
  feed(overlong.c_str()); run(3);                   // 64 bytes per pass: the error comes on the second
  r = take();
  CHECK(count(r, "err line too long") == 1 && replies(r) == 1, "overlong line without newline: one error, got: %s", r.c_str());
  run(250);
  feed("?"); run(210);
  CHECK(replies(take()) == 1, "overflow forgotten after silence, next command works");
  feed("\r\n\n\n");
  run(5);
  CHECK(take().empty(), "blank lines are ignored");
  feed("   \n"); run(1);
  CHECK(err(take()), "a whitespace-only line still gets a reply");
  feed("P 3 200\r\n"); run(1);
  CHECK(ok(take()), "CRLF line ending");
  run(700);
  take();
  feed("P 3"); run(400);                            // a sender that uses newlines is never second-guessed
  CHECK(take().empty(), "partial line waits for its newline once newlines have been seen");
  feed(" 200\n"); run(1);
  r = take();
  CHECK(replies(r) == 1 && r.find("ok P 3 200") != std::string::npos, "and completes as one command: %s", r.c_str());
  run(700);
  take();
  for (int i = 0; i < 10; i++) feed("?\n");         // a flood: one command per pass, tick() keeps running
  run(1);
  CHECK(replies(take()) == 1, "one command handled per loop pass");
  run(9);
  CHECK(replies(take()) == 9, "the rest follow one per pass");
  std::string longline(120, 'P'); longline += "\n";
  feed(longline.c_str()); run(3);
  r = take();
  CHECK(count(r, "err line too long") == 1 && replies(r) == 1, "overlong line: one error, got: %s", r.c_str());
  CHECK(ok(cmd("P 3 200")), "next command works after an overlong line");
  run(700);
  r = cmd("P 1 100 extra words here beyond six tokens total");
  CHECK(ok(r), "extra tokens are ignored: %s", r.c_str());
  run(500);
  r = cmd("?");
  CHECK(count(r, "# ") >= 6 && r.find("ok ?") != std::string::npos, "status lines then ok: %s", r.c_str());
  cmd("A 1");
  r = cmd("D");
  CHECK(ok(r), "demo");
  run(300 + 700 * 9 + 600);
  CHECK(replies(take()) == 0, "demo runs without complaints");
  for (int f = 0; f < 5; f++) CHECK(angle(f) == 0, "all locked after demo");
  CHECK(!anyClosed(), "relays open after demo");

  // ---- millis() wraparound in the middle of a note ------------------------
  g_now = (unsigned long)-1 - 1200;                 // setup takes 1000 ms; wrap lands inside the press
  reboot();
  cmd("A 1");
  t = g_now;
  cmd("P 3 400");
  on = waitClosed(1000);
  CHECK(on == t + 150, "note straddling the wrap closes at t+150");
  off = waitOpen(3000);
  CHECK(off - on == 400 && off < on, "press spans the wrap in full: %lu ms, clock wrapped", off - on);
  run(60);
  CHECK(angle(2) == 0, "locked after the wrap");
  cmd("P 3 300");
  on = waitClosed(1000); off = waitOpen(1000);
  CHECK(off - on == 300, "next note after the wrap is normal");

  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
