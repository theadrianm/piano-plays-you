const int POT_PINS[] = {32, 34, 35, 36, 39};
const char* POT_NAMES[] = {"D32", "D34", "D35", "D36_VP", "D39_VN"};
const int NUM_POTS = sizeof(POT_PINS) / sizeof(POT_PINS[0]);

const int SAMPLES = 8;                // averages several reads to cut noise
const unsigned long PRINT_MS = 100;   // print 10 times per second

int readSmoothed(int pin) {
  long total = 0;
  for (int i = 0; i < SAMPLES; i++) {
    total += analogRead(pin);
  }
  return total / SAMPLES;
}

void setup() {
  Serial.begin(115200);
  while (!Serial);

  analogReadResolution(12);  // values from 0 to 4095

  Serial.println("ESP32 Potentiometer Test Started: turn each pot and watch its value change...");
}

void loop() {
  for (int i = 0; i < NUM_POTS; i++) {
    int value = readSmoothed(POT_PINS[i]);
    Serial.print(POT_NAMES[i]);
    Serial.print(":");
    Serial.print(value);
    if (i < NUM_POTS - 1) {
      Serial.print(" ");
    }
  }
  Serial.println();

  delay(PRINT_MS);
}
