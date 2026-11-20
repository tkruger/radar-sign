# radar-sign

A two-sided "Your Speed" radar sign for streets where kids play.

Each side of the sign has its own 24 GHz radar and a 16×16 LED panel. When a car drives toward the sign, the panel shows the car's speed in big color-coded digits and then a face that tells the driver how they're doing:

| Speed        | Color  | Face        |
|--------------|--------|-------------|
| ≤ 25 mph     | Green  | 🙂 Smiley   |
| 26–29 mph    | Yellow | 😐 Neutral  |
| ≥ 30 mph     | Red    | 🙁 Frown    |

The panel shows the digits for three seconds out of every four and the face for the fourth. If the reading doesn't change for 10 seconds, the panel goes blank.

At power-up, both panels run a self-test: they show `25` and then a green smiley, `29` and a yellow neutral face, and `30` and a red frown, for one second each. Then they go black until the first car is detected. This makes it easy to spot a dead LED or a wiring problem.

## Hardware

| Part | Qty | Notes |
|------|-----|-------|
| Arduino Mega 2560 | 1 | Needed for its multiple hardware UARTs |
| HLK-LD2451 24 GHz vehicle radar | 2 | One per side of the sign |
| 16×16 WS2812B LED matrix | 2 | Serpentine wiring, mounted rotated 180° |
| 5 V power supply | 1 | Size it for 512 LEDs. At the default brightness, plan for at least 5 A |

### Wiring

| Signal | Mega pin |
|--------|----------|
| Front radar TX → | RX3 (pin 15) |
| Front radar RX ← (optional) | TX3 (pin 14) |
| Back radar TX → | RX2 (pin 17) |
| Back radar RX ← (optional) | TX2 (pin 16) |
| Front LED matrix DIN | 6 |
| Back LED matrix DIN | 7 |

Connect every ground (Mega, both radars, LED power supply) together. Power the LED matrices straight from the 5 V supply, not through the Mega. A level shifter is recommended on any line from the Mega to a radar RX pin.

## Building and flashing

1. Install the [Arduino IDE](https://www.arduino.cc/en/software) (or `arduino-cli`).
2. Install the **FastLED** library from the Library Manager.
3. Open `radar-sign.ino`, select **Arduino Mega or Mega 2560**, and upload.
4. Open the Serial Monitor at **115200 baud** to see the decoded radar targets and speeds.

With `arduino-cli`:

```sh
arduino-cli lib install FastLED
arduino-cli compile --fqbn arduino:avr:mega .
arduino-cli upload  --fqbn arduino:avr:mega -p <PORT> .
```

## Configuration

The tunable values are `#define`s near the top of `radar-sign.ino`:

| Setting | Default | What it does |
|---------|---------|--------------|
| `BRIGHTNESS` | `64` | Global LED brightness (0–255) |
| `SHOW_LEADING_ZERO` | `1` | Show `05` rather than ` 5` |
| `MPH_MAX` | `99` | Upper limit for the displayed speed |
| `GREEN_MAX_MPH` / `RED_MIN_MPH` | `25` / `30` | Speed band thresholds (yellow is in between) |
| `RUN_STARTUP_TEST` | `1` | Run the power-up self-test (`0` to skip) |
| `STARTUP_STEP_MS` | `1000` | How long each self-test frame stays up |
| `CLEAR_AFTER_SECONDS` | `10` | Blank a panel after this many seconds with no new reading |
| `ONLY_APPROACHING` | `1` | Ignore cars driving away from the sign |
| `APPROACHING_DIR` | `1` | Radar direction byte value that means "approaching" (see below) |
| `FRONT_LED_PIN` / `BACK_LED_PIN` | `6` / `7` | LED data pins |
| `RadarSerialFront` / `RadarSerialBack` | `Serial3` / `Serial2` | Radar UARTs |

If your matrix is wired or mounted differently, adjust the `XY()` function.

> **Checking the direction setting:** community documentation for the LD2451 says the direction byte is `1` for approaching and `0` for moving away, but this hasn't been checked against the official spec. Walk or drive toward the sign with the Serial Monitor open and watch `radarData.speedDirection`. If approaching targets show `0`, set `APPROACHING_DIR` to `0`.

## How it works

1. **Read the radar:** `parseOneByte()` is a byte-at-a-time state machine that finds LD2451 report frames (`F4 F3 F2 F1` … `F8 F7 F6 F5`). Each radar has its own `FrameParser`, so the two data streams never get mixed.
2. **Decode targets:** `parseRadarPayload()` pulls out angle, distance, direction, speed (km/h) and SNR for each target, and converts the speed to mph.
3. **Pick a speed:** `payloadToSpeedMph()` keeps the fastest approaching target in the frame.
4. **Draw:** `showSpeed()` runs each panel's own timeout and switches between two 7-segment digits (`drawDigit7Seg()`) and a pixel-art face (`drawSmileyGreen()` and the other face functions). Speeds above 99 mph show as `99`.

## Ideas

- **Low-speed filter:** there's a placeholder in `showSpeed()` for hiding very low speeds (pedestrians, noise).
- **Fewer LED refreshes:** `FastLED.show()` updates both panels and runs several times per loop. Calling it once per loop would leave more time for reading the radar UARTs.

## License

[Apache 2.0](LICENSE)
