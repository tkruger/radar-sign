/*
 * ============================================================================
 *  radar-sign.ino
 *  A "Your Speed" radar sign for streets where kids play.
 * ============================================================================
 *
 *  Hardware
 *  --------
 *   - Arduino Mega 2560 (it needs more than one hardware UART)
 *   - 2x HLK-LD2451 24 GHz vehicle radar modules
 *       * Front radar -> Serial3 (Mega RX3 = pin 15, TX3 = pin 14)
 *       * Back radar  -> Serial2 (Mega RX2 = pin 17, TX2 = pin 16)
 *   - 2x 16x16 WS2812B LED matrices (serpentine wiring)
 *       * Front matrix data -> pin 6
 *       * Back matrix data  -> pin 7
 *
 *  What it does
 *  ------------
 *   Each side of the sign has its own radar and its own LED panel. When a
 *   vehicle is detected, the panel shows its speed in MPH as two big
 *   7-segment digits, color-coded:
 *
 *       <= 25 mph   green   + smiley face
 *       26-29 mph   yellow  + neutral face
 *       >= 30 mph   red     + frowning face
 *
 *   The display alternates between the digits and the face (the face takes
 *   one second in every four). If the reading has not changed for 10
 *   seconds, the panel blanks.
 *
 *   At power-up, both panels run a short self-test that cycles through
 *   each band's boundary speed (25, 29, 30) and face, then go black.
 *
 *  Dependencies
 *  ------------
 *   - FastLED (https://github.com/FastLED/FastLED)
 * ============================================================================
 */

#include <Arduino.h>
#include <FastLED.h>

// ==================== Types ====================
// These are declared before anything else so the Arduino IDE's automatic
// prototype generator can see them when it builds function prototypes.

// Top-left corner of one 7x15 pixel "digit cell" on the matrix.
struct DigitBox { uint8_t ox; uint8_t oy; };

// Classic 7-segment naming. Each value is a bit position in DIGIT_MASK:
//
//      AAAAA
//     F     B
//     F     B
//      GGGGG
//     E     C
//     E     C
//      DDDDD
//
enum Seg : uint8_t { SEG_A=6, SEG_B=5, SEG_C=4, SEG_D=3, SEG_E=2, SEG_F=1, SEG_G=0 };

// Which segments light up for each digit 0-9. Bit order is A B C D E F G
// (MSB -> LSB), matching the Seg enum above.
static const uint8_t DIGIT_MASK[10] = {
  0b1111110, // 0
  0b0110000, // 1
  0b1101101, // 2
  0b1111001, // 3
  0b0110011, // 4
  0b1011011, // 5
  0b1011111, // 6
  0b1110000, // 7
  0b1111111, // 8
  0b1111011  // 9
};

// ---- LD2451 frame parser state (one instance per radar) ----
static const uint16_t PAYLOAD_MAX = 256;   // Largest payload we accept

enum ParseState : uint8_t { FIND_HDR, READ_LEN, READ_PAYLOAD, READ_TAIL };

// All the state needed to decode one radar's byte stream.
struct FrameParser {
  ParseState st;
  uint8_t  hdrPos;        // How many header bytes have matched so far
  uint8_t  tailPos;       // How many tail bytes have matched so far
  uint8_t  lenBytes[2];   // Raw length bytes as they arrive
  uint8_t  lenPos;        // How many length bytes have been received
  uint16_t payloadLen;    // Length announced by the current frame
  uint16_t payloadRead;   // Payload bytes received so far
  uint8_t  payload[PAYLOAD_MAX];
};

// ==================== Config ====================
#define RADAR_BAUD 115200      // LD2451 default UART baud rate

#define FRONT_LED_PIN      6   // Data pin for the front-facing matrix
#define BACK_LED_PIN       7   // Data pin for the rear-facing matrix
#define WIDTH        16        // Matrix width in pixels
#define HEIGHT       16        // Matrix height in pixels
#define NUM_LEDS     (WIDTH * HEIGHT)
#define BRIGHTNESS   64        // Global brightness, 0-255. Keep this modest:
                               // 512 LEDs at full white draw a LOT of current.

#define SHOW_LEADING_ZERO 1    // 1 = show "05", 0 = show " 5"
#define MPH_MAX           99   // Two digits only, so clamp the display here

#define ANIMATION_SECONDS 3    // Reserved for future animation timing (unused)

#define CLEAR_AFTER_SECONDS 10 // Blank a panel when its reading hasn't changed for this long

// Speed bands: <= GREEN_MAX_MPH is green, >= RED_MIN_MPH is red, anything
// in between is yellow.
#define GREEN_MAX_MPH 25
#define RED_MIN_MPH   30

// Startup self-test: show each band's top/bottom speed and its face on both
// panels, then go black. Set RUN_STARTUP_TEST to 0 to skip it.
#define RUN_STARTUP_TEST 1
#define STARTUP_STEP_MS  1000  // How long each test frame stays up

// Direction filtering. With ONLY_APPROACHING set to 1, targets moving away
// from the sign are ignored. Community docs for the LD2451 say the direction
// byte is 0x01 for approaching and 0x00 for moving away. If your sign only
// lights up for cars driving AWAY, change APPROACHING_DIR to 0.
#define ONLY_APPROACHING  1
#define APPROACHING_DIR   1

// One pixel buffer per physical panel.
CRGB frontLeds[NUM_LEDS];
CRGB backLeds[NUM_LEDS];

// Hardware UARTs on the Mega used for each radar.
#define RadarSerialFront Serial3
#define RadarSerialBack  Serial2

// One decoded target from an LD2451 report frame.
struct RadarData {
  byte targetQuantity;   // Number of targets the radar says are in the frame
  byte alarmInformation; // Alarm/status byte from the frame header

  byte angle;            // Target angle (raw, per LD2451 protocol)
  byte distance;         // Target distance in meters
  byte speedDirection;   // Movement direction; see APPROACHING_DIR above
  byte speed;            // Speed in km/h as reported by the radar
  byte snr;              // Signal-to-noise ratio

  float speedMph;        // Derived: speed converted from km/h to mph
};

RadarData radarData;     // Only used by the legacy parser at the bottom of the file

// ---- Simple seconds clock (driven by trackTime()) ----
unsigned long currentMillis;
unsigned long previousMillis = 0;
const long interval = 1000;       // ms per "tick" of the seconds counter
unsigned long seconds = 0;        // Seconds since boot

// Per-panel display timers: the value of `seconds` when each panel's speed
// last changed. Each side keeps its own, so a reading on one side doesn't
// keep the other side lit.
unsigned long lastFrontChange = 0;
unsigned long lastBackChange  = 0;
// Panels start out blank and idle; they light up on the first real reading.
bool isFrontClear = true;         // true = front panel is blanked and idle
bool isBackClear  = true;         // true = back panel is blanked and idle


// ==================== Matrix mapping ====================
// Turns (x, y) into an index in the LED strip.
//
// The matrix is wired "serpentine": even rows run left->right, odd rows run
// right->left. The panels are also mounted upside down, so the y axis is
// flipped first. Change this function if your panel is wired differently.
uint16_t XY(uint8_t x, uint8_t y) {
  if (x >= WIDTH || y >= HEIGHT) return 0;

  // Panel is mounted rotated 180 degrees
  y = (HEIGHT - 1) - y;

  if ((y & 1) == 0) return (uint16_t)y * WIDTH + x;     // even row: L -> R
  return (uint16_t)y * WIDTH + (WIDTH - 1 - x);         // odd row:  R -> L
}

// Set every pixel in a buffer to black. (Does not call FastLED.show().)
void clearMatrix(CRGB leds[]) {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
}

// Fill an inclusive rectangle (x0,y0)-(x1,y1) with a color. The corners can
// be given in any order, and the rectangle is clipped to the matrix.
void fillRect(CRGB leds[], uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, const CRGB &c) {
  if (x1 < x0) { uint8_t t=x0; x0=x1; x1=t; }
  if (y1 < y0) { uint8_t t=y0; y0=y1; y1=t; }

  x1 = min(x1, (uint8_t)(WIDTH  - 1));
  y1 = min(y1, (uint8_t)(HEIGHT - 1));

  for (uint8_t y = y0; y <= y1; y++) {
    for (uint8_t x = x0; x <= x1; x++) {
      leds[XY(x, y)] = c;
    }
  }
}

// ==================== 7-segment drawing ====================
// Each digit is drawn in a 7-wide x 15-tall cell with 2-pixel-thick segments,
// so two digits plus a 2-pixel gap fit on the 16x16 panel.

// Draw one segment of a digit cell.
void drawSeg(CRGB leds[], const DigitBox &d, Seg s, const CRGB &c) {
  uint8_t x = d.ox;
  uint8_t y = d.oy;

  // Digit box is 7x15 (x..x+6, y..y+14)
  switch (s) {
    case SEG_A: fillRect(leds, x+1, y+0,  x+5, y+1,  c); break;  // top
    case SEG_G: fillRect(leds, x+1, y+6,  x+5, y+7,  c); break;  // middle
    case SEG_D: fillRect(leds, x+1, y+13, x+5, y+14, c); break;  // bottom

    case SEG_F: fillRect(leds, x+0, y+1,  x+1, y+6,  c); break;  // upper-left
    case SEG_B: fillRect(leds, x+5, y+1,  x+6, y+6,  c); break;  // upper-right
    case SEG_E: fillRect(leds, x+0, y+8,  x+1, y+13, c); break;  // lower-left
    case SEG_C: fillRect(leds, x+5, y+8,  x+6, y+13, c); break;  // lower-right
  }
}

// Draw a full digit (0-9) into a cell. Values above 9 are ignored.
void drawDigit7Seg(CRGB leds[], const DigitBox &d, uint8_t digit, const CRGB &onColor) {
  if (digit > 9) return;
  uint8_t mask = DIGIT_MASK[digit];

  if (mask & (1 << SEG_A)) drawSeg(leds, d, SEG_A, onColor);
  if (mask & (1 << SEG_B)) drawSeg(leds, d, SEG_B, onColor);
  if (mask & (1 << SEG_C)) drawSeg(leds, d, SEG_C, onColor);
  if (mask & (1 << SEG_D)) drawSeg(leds, d, SEG_D, onColor);
  if (mask & (1 << SEG_E)) drawSeg(leds, d, SEG_E, onColor);
  if (mask & (1 << SEG_F)) drawSeg(leds, d, SEG_F, onColor);
  if (mask & (1 << SEG_G)) drawSeg(leds, d, SEG_G, onColor);
}

// ==================== Display state machine ====================
// Draws one panel. Called every pass through loop() for each panel.
//
//   isClear    - per-panel flag: true means the panel has timed out and is blank
//   lastChange - per-panel timer: `seconds` value when the speed last changed
//   leds       - that panel's pixel buffer
//   mph        - latest speed from the radar (may be reset to 0 here)
//   lastMph    - last speed shown, used to spot a new reading
//
// Behavior:
//   - A new speed restarts this panel's display timer.
//   - After CLEAR_AFTER_SECONDS with no change, the panel blanks and stays
//     blank until the next new reading.
//   - While active, one second in four shows a face and the other three
//     show the speed as colored digits.
//
// NOTE: FastLED.show() pushes ALL registered panels at once, not just the
// panel whose buffer is passed in here.
void showSpeed(bool &isClear, unsigned long &lastChange, CRGB leds[], float &mph, float &lastMph) {
  clearMatrix(leds);

  if (lastMph != mph){
    // New reading: restart this panel's display window
    Serial.println("Velocity changed, restarting display timer");
    lastChange = seconds;
    lastMph = mph;
    isClear = false;
  } else if (isClear){
    // Already blanked and nothing new: leave the (cleared) buffer as-is
    return;
  }

  if (seconds - lastChange >= CLEAR_AFTER_SECONDS) {
    // Timed out: blank the panel and reset the speed values
    Serial.println("Clearing after timeout");
    isClear = true;
    mph = 0;
    lastMph = 0;
    FastLED.show();
    return;
  }

  // Clamp to what two digits can show, then round. Clamping a local copy
  // keeps `mph`/`lastMph` untouched so change detection still works.
  float shownMph = mph;
  if (shownMph < 0) shownMph = 0;
  if (shownMph > MPH_MAX) shownMph = MPH_MAX;
  uint8_t mphInt = (uint8_t)lroundf(shownMph);

  if (mphInt <= 2){
    // FUTURE: treat very low speeds (pedestrians, noise) as "no target"
  }

  if (seconds % 4 == 0){
    drawFaceForSpeed(leds, mphInt);    // face frame (1 second in 4)
  } else {
    drawSpeedDigits(leds, mphInt);     // digits frame
  }
  FastLED.show();
}

// Draw a 0-99 speed as two colored 7-segment digits. Does not call show().
void drawSpeedDigits(CRGB leds[], uint8_t mphInt) {
  clearMatrix(leds);
  uint8_t tens = mphInt / 10;
  uint8_t ones = mphInt % 10;

  // Alternative: smooth green -> red gradient based on speed
  //uint8_t hue = (uint8_t)map(mphInt, 0, MPH_MAX, 96, 0);
  //CRGB onColor = CHSV(hue, 255, 255);
  CRGB onColor;

  if (mphInt <= GREEN_MAX_MPH) {
    onColor = CHSV(96, 255, 180);   // green
  }
  else if (mphInt < RED_MIN_MPH) {
    onColor = CHSV(40, 255, 200);   // yellow / amber
  }
  else {
    onColor = CHSV(0, 255, 220);    // red
  }

  DigitBox left  = {0, 0};  // x=0..6,  y=0..14
  DigitBox right = {9, 0};  // x=9..15, y=0..14 (gap at x=7..8)

  if (SHOW_LEADING_ZERO || tens != 0) drawDigit7Seg(leds, left, tens, onColor);
  drawDigit7Seg(leds, right, ones, onColor);
}

// Draw the face that matches a speed's band. Does not call show().
void drawFaceForSpeed(CRGB leds[], uint8_t mphInt) {
  if (mphInt <= GREEN_MAX_MPH) drawSmileyGreen(leds);
  else if (mphInt < RED_MIN_MPH) drawNeutralYellow(leds);
  else drawFrownRed(leds);
}

// ==================== Startup self-test ====================
// Runs once at boot. Steps through each speed band on both panels, showing
// the band's boundary speed and then its face, so you can see that every
// LED, color and face works and where each threshold sits. Then it blanks
// both panels.
void runStartupTest() {
  const uint8_t testSpeeds[] = {
    GREEN_MAX_MPH,      // fastest speed that's still green
    RED_MIN_MPH - 1,    // fastest yellow speed
    RED_MIN_MPH         // slowest red speed
  };

  for (uint8_t i = 0; i < sizeof(testSpeeds); i++) {
    uint8_t mph = testSpeeds[i];

    drawSpeedDigits(frontLeds, mph);
    drawSpeedDigits(backLeds,  mph);
    FastLED.show();
    delay(STARTUP_STEP_MS);

    drawFaceForSpeed(frontLeds, mph);
    drawFaceForSpeed(backLeds,  mph);
    FastLED.show();
    delay(STARTUP_STEP_MS);
  }

  clearMatrix(frontLeds);
  clearMatrix(backLeds);
  FastLED.show();
}

// ==================== Face graphics ====================
// Simple pixel-art faces drawn on the 16x16 grid: a thick colored ring for
// the head, two 2x2 white eyes, and a white mouth.

// Set a pixel, silently ignoring anything off the panel.
static inline void setPixelSafe(CRGB leds[], int x, int y, const CRGB& c) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
  leds[XY((uint8_t)x, (uint8_t)y)] = c;
}

// Draw a hollow ring `thickness` pixels wide that fills the panel.
static void drawFaceOutline(CRGB leds[], const CRGB& faceColor, uint8_t thickness = 2) {
  // Centered between pixels (7.5, 7.5) so the circle is symmetric on an
  // even-sized grid.
  const float cx = 7.5f;
  const float cy = 7.5f;

  // Largest radius that fits on 16x16 with a half-pixel center
  const float rOuter = 8.0f;
  float rInner = rOuter - thickness;
  if (rInner < 0) rInner = 0;

  // Compare squared distances to avoid sqrt()
  const float rOuter2 = rOuter * rOuter;
  const float rInner2 = rInner * rInner;

  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      float dx = x - cx;
      float dy = y - cy;
      float d2 = dx*dx + dy*dy;

      // Pixel lies within the ring
      if (d2 <= rOuter2 && d2 >= rInner2) {
        setPixelSafe(leds, x, y, faceColor);
      }
    }
  }
}

// Two 2x2 eyes.
static void drawEyes(CRGB leds[], const CRGB& eyeColor) {
  // Left eye (x=4..5, y=5..6)
  setPixelSafe(leds, 4, 5, eyeColor); setPixelSafe(leds, 5, 5, eyeColor);
  setPixelSafe(leds, 4, 6, eyeColor); setPixelSafe(leds, 5, 6, eyeColor);

  // Right eye (x=10..11, y=5..6)
  setPixelSafe(leds, 10, 5, eyeColor); setPixelSafe(leds, 11, 5, eyeColor);
  setPixelSafe(leds, 10, 6, eyeColor); setPixelSafe(leds, 11, 6, eyeColor);
}

// ---- Mouth styles ----

// Smile: the corners sit higher than the middle (rows 10-12).
static void drawMouthSmile(CRGB leds[], const CRGB& mouthColor) {
  setPixelSafe(leds, 4, 10, mouthColor);
  setPixelSafe(leds, 5, 11, mouthColor);
  setPixelSafe(leds, 6, 12, mouthColor);
  setPixelSafe(leds, 7, 12, mouthColor);
  setPixelSafe(leds, 8, 12, mouthColor);
  setPixelSafe(leds, 9, 12, mouthColor);
  setPixelSafe(leds, 10, 11, mouthColor);
  setPixelSafe(leds, 11, 10, mouthColor);
}

// Neutral: a flat line two pixels thick (rows 10-11).
static void drawMouthNeutral(CRGB leds[], const CRGB& mouthColor) {
  for (int x = 4; x <= 11; x++) setPixelSafe(leds, x, 10, mouthColor);
  for (int x = 4; x <= 11; x++) setPixelSafe(leds, x, 11, mouthColor);
}

// Frown: an upside-down smile, with the middle higher than the corners (rows 9-11).
static void drawMouthFrown(CRGB leds[], const CRGB& mouthColor) {
  setPixelSafe(leds, 4, 11, mouthColor);
  setPixelSafe(leds, 5, 10, mouthColor);
  setPixelSafe(leds, 6, 9, mouthColor);
  setPixelSafe(leds, 7, 9, mouthColor);
  setPixelSafe(leds, 8, 9, mouthColor);
  setPixelSafe(leds, 9, 9, mouthColor);
  setPixelSafe(leds, 10, 10, mouthColor);
  setPixelSafe(leds, 11, 11, mouthColor);
}

// ---- Complete faces (each clears the buffer and draws; the caller calls show()) ----

void drawSmileyGreen(CRGB leds[]) {
  clearMatrix(leds);
  drawFaceOutline(leds, CHSV(96, 255, 220));   // green outline
  drawEyes(leds, CRGB::White);
  drawMouthSmile(leds, CRGB::White);
}

void drawNeutralYellow(CRGB leds[]) {
  clearMatrix(leds);
  drawFaceOutline(leds, CHSV(40, 255, 240));   // yellow outline
  drawEyes(leds, CRGB::White);
  drawMouthNeutral(leds, CRGB::White);
}

void drawFrownRed(CRGB leds[]) {
  clearMatrix(leds);
  drawFaceOutline(leds, CHSV(0, 255, 240));    // red outline
  drawEyes(leds, CRGB::White);
  drawMouthFrown(leds, CRGB::White);
}


// ==================== LD2451 frame parser ====================
// The LD2451 sends report frames shaped like this:
//
//   [F4 F3 F2 F1] [len_lo len_hi] [payload ... len bytes] [F8 F7 F6 F5]
//      header       little-endian                            tail
//
// parseOneByte() is a small state machine that takes one byte at a time and
// returns true when a full, valid frame has landed in the parser's `payload`.
//
// Each radar gets its own FrameParser, so a half-received frame from one
// radar can never be mixed up with bytes from the other.

// (FrameParser and ParseState are declared in the Types section at the top.)

static const uint8_t HDR[4]  = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t TAIL[4] = {0xF8, 0xF7, 0xF6, 0xF5};

// Zero-initialized globals, so every parser starts in FIND_HDR.
FrameParser frontParser;
FrameParser backParser;

// Feed one byte to a parser. Returns true when a full frame has been received.
bool parseOneByte(FrameParser &p, uint8_t b) {
  // Uncomment to dump raw bytes in hex for debugging:
  //if (b < 0x10) Serial.print("0");
  //Serial.print(b, HEX);
  switch (p.st) {
    case FIND_HDR:
      // Look for the 4-byte header; start over on any mismatch
      if (b == HDR[p.hdrPos]) {
        p.hdrPos++;
        if (p.hdrPos == 4) { p.hdrPos = 0; p.lenPos = 0; p.st = READ_LEN; }
      } else {
        // A mismatched byte might itself be the first header byte
        p.hdrPos = (b == HDR[0]) ? 1 : 0;
      }
      break;

    case READ_LEN:
      // Two-byte little-endian payload length
      p.lenBytes[p.lenPos++] = b;
      if (p.lenPos == 2) {
        p.lenPos = 0;
        p.payloadLen = (uint16_t)p.lenBytes[0] | ((uint16_t)p.lenBytes[1] << 8);
        p.payloadRead = 0;
        // Reject lengths that would overflow the buffer
        if (p.payloadLen > PAYLOAD_MAX) { p.st = FIND_HDR; return false; }
        // A zero-length payload goes straight to the tail
        p.tailPos = 0;
        p.st = (p.payloadLen == 0) ? READ_TAIL : READ_PAYLOAD;
      }
      break;

    case READ_PAYLOAD:
      p.payload[p.payloadRead++] = b;
      if (p.payloadRead >= p.payloadLen) { p.tailPos = 0; p.st = READ_TAIL; }
      break;

    case READ_TAIL:
      // The frame is valid only if all 4 tail bytes match
      if (b == TAIL[p.tailPos]) {
        p.tailPos++;
        if (p.tailPos == 4) { p.st = FIND_HDR; p.tailPos = 0; return true; }
      } else { p.st = FIND_HDR; p.tailPos = 0; }
      break;
  }
  return false;
}

// Payload layout:
//   [0]      target count
//   [1]      alarm info
//   [2 + 5*i ...] one 5-byte block per target:
//            angle, distance, direction, speed (km/h), SNR
static const uint8_t BYTES_PER_TARGET = 5;
static const uint8_t PAYLOAD_HEADER_BYTES = 2;  // targetQuantity + alarmInformation

// Decode up to `maxTargets` targets from a payload into `outTargets`.
// Returns the number decoded, which is capped by the announced count, the
// bytes actually present, and `maxTargets`.
uint8_t parseRadarPayload(const uint8_t* payloadBuf,
                          uint16_t payloadLen,
                          RadarData* outTargets,
                          uint8_t maxTargets)
{
  if (!payloadBuf || !outTargets) return 0;
  if (payloadLen < PAYLOAD_HEADER_BYTES) return 0;

  uint8_t targetQty = payloadBuf[0];
  uint8_t alarmInfo = payloadBuf[1];

  // Never trust targetQty alone; also limit by the bytes received
  uint16_t available = payloadLen - PAYLOAD_HEADER_BYTES;
  uint8_t targetsInFrame = (uint8_t)(available / BYTES_PER_TARGET);

  uint8_t count = targetQty;
  if (count > targetsInFrame) count = targetsInFrame;
  if (count > maxTargets) count = maxTargets;

  for (uint8_t i = 0; i < count; i++) {
    uint16_t base = PAYLOAD_HEADER_BYTES + (uint16_t)i * BYTES_PER_TARGET;

    RadarData rd;
    rd.targetQuantity   = targetQty;
    rd.alarmInformation = alarmInfo;

    rd.angle          = payloadBuf[base + 0];
    rd.distance       = payloadBuf[base + 1];
    rd.speedDirection = payloadBuf[base + 2];
    rd.speed          = payloadBuf[base + 3];   // km/h
    rd.snr            = payloadBuf[base + 4];

    // km/h -> mph, converted once here
    rd.speedMph = rd.speed * 0.621371f;

    // Debug dump of every decoded target
    Serial.print("radarData.targetQuantity ");
    Serial.println(rd.targetQuantity);
    Serial.print("radarData.alarmInformation ");
    Serial.println(rd.alarmInformation);
    Serial.print("radarData.angle ");
    Serial.println(rd.angle);
    Serial.print("radarData.distance ");
    Serial.println(rd.distance);
    Serial.print("radarData.speedDirection ");
    Serial.println(rd.speedDirection);
    Serial.print("radarData.speed ");
    Serial.println(rd.speed);
    Serial.print("radarData.snr ");
    Serial.println(rd.snr);
    Serial.print("radarData.speedMph ");
    Serial.println(rd.speedMph);

    outTargets[i] = rd;
  }

  return count;
}

// Turn the frame a parser just completed into one speed to display.
// Returns false only when the payload is too short to be a report.
//
// Policy: show the FASTEST target in the frame. With ONLY_APPROACHING set,
// targets moving away from the sign are skipped.
bool payloadToSpeedMph(const FrameParser &p, float &outMph) {
  if (p.payloadLen < 2) return false;

  const uint8_t MAX_TARGETS = 8;
  RadarData targets[MAX_TARGETS];

  uint8_t count = parseRadarPayload(p.payload, p.payloadLen, targets, MAX_TARGETS);
  if (count == 0) {
    outMph = 0.0f;
    return true; // valid frame, just no targets
  }

  float maxMph = 0.0f;
  for (uint8_t i = 0; i < count; i++) {
    if (ONLY_APPROACHING && targets[i].speedDirection != APPROACHING_DIR) continue;
    if (targets[i].speedMph > maxMph) {
      maxMph = targets[i].speedMph;
    }
  }

  outMph = maxMph;
  return true;
}

/*
 * ---------------------------------------------------------------------------
 * LEGACY: the first version of payloadToSpeedMph(), kept for reference.
 * Its per-target byte offsets were wrong; parseRadarPayload() above
 * replaces it.
 * ---------------------------------------------------------------------------
bool payloadToSpeedMph(float &outMph) {
  if (payloadLen < 2) return false;
  for(int i = 0; i < payloadLen; i++)
  {
    if (payload[i] < 0x10) Serial.print("0");
    Serial.print(payload[i],HEX);
  }
  Serial.println("");
  // Store data in the RadarData structure (assuming only one target for now)
  radarData.targetQuantity = payload[0];

  uint16_t needed = 2 + (uint16_t)radarData.targetQuantity * 5;
  if (payloadLen < needed) return false;

  float maxMph = 0.0f;
  for (uint8_t i = 0; i < radarData.targetQuantity; i++) {
    uint16_t base = 2 + (uint16_t)i * 5;
    uint8_t spKmh = payload[base + 3];       // speed in km/h (per LD2451 reporting)

    radarData.alarmInformation = payload[base + 1];
    radarData.angle = payload[base + 2];
    radarData.distance = payload[base + 3];
    radarData.speedDirection = payload[base + 4];
    radarData.speed = payload[base + 5];
    radarData.snr = payload[base + 6];
    Serial.print("radarData.alarmInformation ");
    Serial.println(radarData.alarmInformation);
    Serial.print("radarData.angle ");
    Serial.println(radarData.angle);
    Serial.print("radarData.distance ");
    Serial.println(radarData.distance);
    Serial.print("radarData.speed ");
    Serial.println(radarData.speed);
    Serial.print("radarData.snr ");
    Serial.println(radarData.snr);

    float mph = spKmh * 0.621371f;           // km/h -> mph
    if (mph > maxMph) maxMph = mph;
  }
  outMph = maxMph;
  return true;
}
*/

// ==================== Timekeeping ====================
// Adds 1 to `seconds` each time 1000 ms have passed, without blocking.
// millis() wraps after about 49 days; the unsigned subtraction handles that.
void trackTime(){
  currentMillis = millis();

  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    seconds++;
  }
}

// ==================== Setup / Loop ====================
void setup() {
  Serial.begin(115200);          // USB serial for debug output
  delay(200);

  // Register both panels with FastLED. A call to FastLED.show() refreshes both.
  FastLED.addLeds<WS2812B, FRONT_LED_PIN, GRB>(frontLeds, NUM_LEDS);
  FastLED.addLeds<WS2812B, BACK_LED_PIN, GRB>(backLeds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  clearMatrix(frontLeds);
  clearMatrix(backLeds);
  FastLED.show();

#if RUN_STARTUP_TEST
  Serial.println("Running startup self-test...");
  runStartupTest();   // ends with both panels black
#endif

  // Start the radars after the self-test so stale bytes don't pile up in
  // the UART buffers while it runs.
  RadarSerialFront.begin(RADAR_BAUD);
  RadarSerialBack.begin(RADAR_BAUD);

  Serial.println("Mega2560: 2x LD2451 -> MPH -> 2-digit 7seg on 2x 16x16 matrices.");
  Serial.println("Wiring: Front LD2451 TX->Mega15(RX3), Back LD2451 TX->Mega17(RX2), GND->GND, VCC->5V.");
}

// Latest reading and last-displayed reading for each side of the sign.
float frontMph = 0.0f;
float lastFrontMph = 0.0f;
float backMph = 0.0f;
float lastBackMph = 0.0f;

void loop() {
  trackTime();

  // Read every byte waiting from the front radar
  while (RadarSerialFront.available() > 0) {
    uint8_t b = (uint8_t)RadarSerialFront.read();
    if (parseOneByte(frontParser, b)) {
      if (payloadToSpeedMph(frontParser, frontMph)) {
        Serial.print("Speed Front: ");
        Serial.print(frontMph, 1);
        Serial.println(" mph");
      }
    }
  }

  // Read every byte waiting from the back radar
  while (RadarSerialBack.available() > 0) {
    uint8_t b = (uint8_t)RadarSerialBack.read();
    if (parseOneByte(backParser, b)) {
      if (payloadToSpeedMph(backParser, backMph)) {
        Serial.print("Speed Back: ");
        Serial.print(backMph, 1);
        Serial.println(" mph");
      }
    }
  }

  // Redraw both panels
  showSpeed(isFrontClear, lastFrontChange, frontLeds, frontMph, lastFrontMph);
  showSpeed(isBackClear,  lastBackChange,  backLeds,  backMph,  lastBackMph);
}
