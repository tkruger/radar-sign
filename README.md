# radar-sign

A two-sided "Your Speed" radar sign for streets where kids play.

Each side of the sign has its own 24 GHz radar and a 16×16 LED panel. When a car drives toward the sign, the panel shows the car's speed in big color-coded digits and then a face that tells the driver how they're doing:

| Speed        | Color  | Face        |
|--------------|--------|-------------|
| ≤ 25 mph     | Green  | 🙂 Smiley   |
| 26–29 mph    | Yellow | 😐 Neutral  |
| ≥ 30 mph     | Red    | 🙁 Frown    |

The panel shows the digits for three seconds out of every four and the face for the fourth. If the reading doesn't change for 10 seconds, the panel goes blank.

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
| `FRONT_LED_PIN` / `BACK_LED_PIN` | `6` / `7` | LED data pins |
| `RadarSerialFront` / `RadarSerialBack` | `Serial3` / `Serial2` | Radar UARTs |

The speed limits for the green, yellow and red bands (25 / 30 mph) are set in `showSpeed()`. If your matrix is wired or mounted differently, adjust the `XY()` function.

## How it works

1. **Read the radar:** `parseOneByte()` is a byte-at-a-time state machine that finds LD2451 report frames (`F4 F3 F2 F1` … `F8 F7 F6 F5`).
2. **Decode targets:** `parseRadarPayload()` pulls out angle, distance, direction, speed (km/h) and SNR for each target, and converts the speed to mph.
3. **Pick a speed:** `payloadToSpeedMph()` keeps the fastest target in the frame.
4. **Draw:** `showSpeed()` handles the 10-second timeout and switches between two 7-segment digits (`drawDigit7Seg()`) and a pixel-art face (`drawSmileyGreen()` and the other face functions).

## Known limitations and ideas

These are also marked with `NOTE:` comments in the code:

- **Shared parser state:** both radars feed the same frame parser. If a frame from one radar is only half received when the other radar's bytes arrive, the two streams can corrupt each other. A parser state per radar would fix this.
- **Shared timeout clock:** the 10-second timer (`last`) is shared, so a new reading on one side also keeps the other side lit.
- **Direction is ignored:** cars driving away are shown too. `speedDirection` is available if you only want to show approaching traffic.
- **The speed clamp runs too late:** speeds of 100 mph and above show a blank tens digit instead of `99`.
- **Low-speed filter:** there's a placeholder for hiding very low speeds (pedestrians, noise) that isn't implemented yet.

## License

[Apache 2.0](LICENSE)
