// Minimal Arduino stand-in so glove.ino can be compiled and driven on a laptop.
// Time is a counter the test advances by hand; pins, servo angles and the
// serial port are plain variables the test can inspect.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <strings.h>

#define LOW 0
#define HIGH 1
#define INPUT 1
#define OUTPUT 3
#define INPUT_PULLUP 5
#define INPUT_PULLDOWN 9

extern unsigned long g_now;          // simulated millis()
extern int g_pins[64];               // last digitalWrite per pin, -1 = never written
extern int g_pinMode[64];
extern uint32_t g_ledcDuty[64];      // last ledcWrite per pin, 0 = never written

inline unsigned long millis() { return g_now; }
inline void delay(unsigned long ms) { g_now += ms; }
inline void pinMode(int pin, int mode) { g_pinMode[pin] = mode; }
inline void digitalWrite(int pin, int val) { g_pins[pin] = val; }
inline bool ledcAttach(uint8_t pin, uint32_t, uint8_t) { g_pinMode[pin] = 100; return true; }
inline bool ledcWrite(uint8_t pin, uint32_t duty) { g_ledcDuty[pin] = duty; return true; }
inline bool ledcDetach(uint8_t pin) { g_ledcDuty[pin] = 0; g_pinMode[pin] = -1; return true; }

struct FakeSerial {
  std::string in;    // bytes the sketch has not read yet
  std::string out;   // everything the sketch printed
  void begin(unsigned long) {}
  int available() { return (int)in.size(); }
  int read() {
    if (in.empty()) return -1;
    int c = (unsigned char)in[0];
    in.erase(0, 1);
    return c;
  }
  void print(char c) { out += c; }
  void print(const char* s) { out += s; }
  template <class T> void print(T v) { std::ostringstream ss; ss << v; out += ss.str(); }
  template <class T> void println(T v) { print(v); out += '\n'; }
  void println() { out += '\n'; }
};
extern FakeSerial Serial;
