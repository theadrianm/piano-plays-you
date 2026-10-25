#pragma once
#include "Arduino.h"

extern int g_servoAngle[64];   // last angle written per GPIO, -1 = never

class Servo {
 public:
  int pin = -1;
  void setPeriodHertz(int) {}
  int attach(int p, int, int) { pin = p; return 1; }
  void write(int angle) { g_servoAngle[pin] = angle; }
};

class ESP32PWM {
 public:
  static void allocateTimer(int) {}
};
