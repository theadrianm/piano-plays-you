#include <ESP32Servo.h>

#define LED_PIN 2  // built-in LED, on while a servo is moving

const int SERVO_PINS[] = {18, 19, 21, 22, 23};
const int NUM_SERVOS = sizeof(SERVO_PINS) / sizeof(SERVO_PINS[0]);

const int MIN_PULSE_US = 500;   // pulse width for 0 degrees (SG90/MG90S)
const int MAX_PULSE_US = 2400;  // pulse width for 180 degrees

const int ANGLE_REST = 0;
const int ANGLE_ACTIVE = 180;

const unsigned long HOLD_MS = 2000;  // how long each servo holds at 180
const unsigned long GAP_MS = 500;    // pause after returning to 0

Servo servos[NUM_SERVOS];

void setup() {
  Serial.begin(115200);
  while (!Serial);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  for (int i = 0; i < NUM_SERVOS; i++) {
    servos[i].setPeriodHertz(50);
    servos[i].attach(SERVO_PINS[i], MIN_PULSE_US, MAX_PULSE_US);
    servos[i].write(ANGLE_REST);
    Serial.print("Attached servo on GPIO ");
    Serial.println(SERVO_PINS[i]);
    delay(200);  // stagger startup so all 5 don't jerk at once
  }

  Serial.println("ESP32 Servo Test Started: sweeping servos one at a time...");
}

void loop() {
  for (int i = 0; i < NUM_SERVOS; i++) {
    int pin = SERVO_PINS[i];

    servos[i].write(ANGLE_ACTIVE);
    digitalWrite(LED_PIN, HIGH);
    Serial.print("GPIO ");
    Serial.print(pin);
    Serial.println(" -> 180");

    delay(HOLD_MS);

    servos[i].write(ANGLE_REST);
    digitalWrite(LED_PIN, LOW);
    Serial.print("GPIO ");
    Serial.print(pin);
    Serial.println(" -> 0");

    delay(GAP_MS);
  }

  Serial.println("--- cycle complete ---");
}
