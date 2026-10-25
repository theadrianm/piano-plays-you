# Piano Plays You

A haptic glove that plays the piano with your hand. Servos hold back the fingers that shouldn't move, and EMS (a TENS unit) drives the ones that should. The judge picks the song, so nothing is pre-rehearsed.

## Firmware

ESP32 (ESP-WROOM-32) sketches, one folder per sketch so the Arduino IDE can open them directly.

| Sketch | What it does |
|---|---|
| `firmware/servo_test` | Sweeps each of the 5 servos to 180° and back, one at a time (built-in LED on while one is moving). Prints `--- cycle complete ---` every ~12.5 s. A servo that doesn't move on its turn has a bad signal wire or is dead. |
| `firmware/pot_test` | Reads the 5 finger potentiometers and prints smoothed values (0-4095) 10x per second. Works with the Serial Plotter. |

### Setup

1. Arduino IDE with the **esp32** board package, board set to **ESP32 Dev Module**.
2. Install the **ESP32Servo** library (Library Manager).
3. Serial Monitor at **115200** baud.

### Pins

| Finger | Servo GPIO | Pot GPIO |
|---|---|---|
| 1 | 18 | 32 |
| 2 | 19 | 34 |
| 3 | 21 | 35 |
| 4 | 22 | 36 (VP) |
| 5 | 23 | 39 (VN) |

The pot pins are all on ADC1, so they keep working when WiFi is on.

## Power notes

- **Plug the battery in after uploading.** Power banks shut off when the draw is very low. During an upload the servos draw almost nothing, so the bank turns itself off. Once the ESP32 also runs off the battery (through VIN), its steady draw keeps the bank awake.
- Don't put the battery on VIN while the laptop USB is plugged in. While developing, use the battery for the servos only.
- Keep the **1000 µF capacitor** across the servo power rail.
- 5 V / 3 A is enough for normal moves. All 5 servos stalled at once (pushing against fingers) can briefly go over 3 A and brown out, so avoid holding all 5 at full resistance together.
- A phone works as a power source for testing, but it only gives ~0.9 A. Don't use it for the demo.
