#include <Arduino.h>
#include <FastLED.h>

// ==================== Types FIRST ====================
struct DigitBox { uint8_t ox; uint8_t oy; };

enum Seg : uint8_t { SEG_A=6, SEG_B=5, SEG_C=4, SEG_D=3, SEG_E=2, SEG_F=1, SEG_G=0 };

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

// ==================== Config ====================
#define RADAR_BAUD 115200

#define FRONT_LED_PIN      6
#define BACK_LED_PIN      7
#define WIDTH        16
#define HEIGHT       16
#define NUM_LEDS     (WIDTH * HEIGHT)
#define BRIGHTNESS   64

#define SHOW_LEADING_ZERO 1
#define MPH_MAX           99

#define ANIMATION_SECONDS 3

CRGB frontLeds[NUM_LEDS];
CRGB backLeds[NUM_LEDS];

// Use Mega hardware UART1
#define RadarSerialFront Serial3
#define RadarSerialBack Serial2

// Structure to hold radar data (organized and self-contained)
struct RadarData {
  byte targetQuantity;
  byte alarmInformation;

  byte angle;
  byte distance;
  byte speedDirection;  // 0 = toward, 1 = away (per LD2451 docs)
  byte speed;           // km/h from radar
  byte snr;             // signal-to-noise ratio

  float speedMph;       // derived value (km/h → mph)
};

RadarData radarData;

unsigned long currentMillis;
unsigned long previousMillis = 0;
const long interval = 1000; // interval at which to track seconds (1000 milliseconds = 1 second)
unsigned long seconds = 0;   // variable to store the number of seconds
unsigned long last = 0; // Last change shown
unsigned long isFrontClear = 0; // Is the sign clear?
unsigned long isBackClear = 0; // Is the sign clear?


// ==================== Matrix mapping ====================
// Serpentine 16x16 mapping (adjust if your matrix is different)
uint16_t XY(uint8_t x, uint8_t y) {
  if (x >= WIDTH || y >= HEIGHT) return 0;

  // Rotate 180
  y = (HEIGHT - 1) - y;

  if ((y & 1) == 0) return (uint16_t)y * WIDTH + x;
  return (uint16_t)y * WIDTH + (WIDTH - 1 - x);
}


void clearMatrix(CRGB leds[]) {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
}

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

// ==================== 7-seg drawing ====================
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

void showSpeed(unsigned long &isClear, CRGB leds[], float &mph, float &lastMph) {
  clearMatrix(leds);
  uint8_t mphInt = (uint8_t)lroundf(mph);

  if (mphInt <= 2){
    //Serial.println("FUTURE: Clear under 2mph");
  }
  if (lastMph != mph){
    //update on change of velocity
    Serial.println("Velocity changed, showing for 10 more seconds");
    last = seconds;
    lastMph = mph;
    isClear = 0;
  } else if (isClear == 1){
    return;
  }
  if (seconds - last >= 10 && isClear != 1) {
    // reset the sign after 10 seconds
    Serial.println("Clearing after 10 seconds");
    isClear = 1;
    mph = 0;
    lastMph = 0;
    FastLED.show();
    return;
  }

  if (mph < 0) mph = 0;
  if (mph > MPH_MAX) mph = MPH_MAX;

  //Serial.print("Sigh speed: ");
  //Serial.println(mphInt);
  uint8_t tens = mphInt / 10;
  uint8_t ones = mphInt % 10;


  if (seconds % 4 == 0){
  
    if (mphInt <= 25) drawSmileyGreen(leds);
    else if (mphInt < 30) drawNeutralYellow(leds);
    else drawFrownRed(leds);  
  
  } else {
    // Green->Red hue based on speed
    //uint8_t hue = (uint8_t)map(mphInt, 0, MPH_MAX, 96, 0);
    //CRGB onColor = CHSV(hue, 255, 255);
    CRGB onColor;

    if (mphInt <= 25) {
      onColor = CHSV(96, 255, 180);
    }
    else if (mphInt < 30) {
      onColor = CHSV(40, 255, 200);
    }
    else {
      onColor = CHSV(0, 255, 220);
    }

    DigitBox left  = {0, 0};  // x=0..6, y=0..14
    DigitBox right = {9, 0};  // x=9..15, y=0..14 (gap x=7..8)

    if (SHOW_LEADING_ZERO || tens != 0) drawDigit7Seg(leds, left, tens, onColor);
    drawDigit7Seg(leds, right, ones, onColor);

    FastLED.show();
  }
}

// Draw a filled circle-ish face + eyes + mouth on a 16x16 matrix.
// Assumes you already have:
// - CRGB leds[NUM_LEDS];
// - uint16_t XY(uint8_t x, uint8_t y);
// - clearMatrix();
// - FastLED.show();

static inline void setPixelSafe(CRGB leds[], int x, int y, const CRGB& c) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
  leds[XY((uint8_t)x, (uint8_t)y)] = c;
}

static void drawFaceOutline(CRGB leds[], const CRGB& faceColor, uint8_t thickness = 2) {
  // Center between pixels for better symmetry
  const float cx = 7.5f;
  const float cy = 7.5f;

  // Maximum usable radius on 16x16 with half-pixel center
  const float rOuter = 8.0f;              // pushes close to edges
  float rInner = rOuter - thickness;
  if (rInner < 0) rInner = 0;

  const float rOuter2 = rOuter * rOuter;
  const float rInner2 = rInner * rInner;

  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      float dx = x - cx;
      float dy = y - cy;
      float d2 = dx*dx + dy*dy;

      // Thick hollow ring
      if (d2 <= rOuter2 && d2 >= rInner2) {
        setPixelSafe(leds, x, y, faceColor);
      }
    }
  }
}


static void drawEyes(CRGB leds[], const CRGB& eyeColor) {
  // Two 2x2 eyes
  // Left eye (x=4..5, y=5..6)
  setPixelSafe(leds, 4, 5, eyeColor); setPixelSafe(leds, 5, 5, eyeColor);
  setPixelSafe(leds, 4, 6, eyeColor); setPixelSafe(leds, 5, 6, eyeColor);

  // Right eye (x=10..11, y=5..6)
  setPixelSafe(leds, 10, 5, eyeColor); setPixelSafe(leds, 11, 5, eyeColor);
  setPixelSafe(leds, 10, 6, eyeColor); setPixelSafe(leds, 11, 6, eyeColor);
}

// Mouth styles
static void drawMouthSmile(CRGB leds[], const CRGB& mouthColor) {
  // A simple upward curve
  // Ends a bit higher than center, middle lower
  // y coordinates: 10-12 region
  setPixelSafe(leds, 4, 10, mouthColor);
  setPixelSafe(leds, 5, 11, mouthColor);
  setPixelSafe(leds, 6, 12, mouthColor);
  setPixelSafe(leds, 7, 12, mouthColor);
  setPixelSafe(leds, 8, 12, mouthColor);
  setPixelSafe(leds, 9, 12, mouthColor);
  setPixelSafe(leds, 10, 11, mouthColor);
  setPixelSafe(leds, 11, 10, mouthColor);

}

static void drawMouthNeutral(CRGB leds[], const CRGB& mouthColor) {
  // Straight mouth line at y=11
  for (int x = 4; x <= 11; x++) setPixelSafe(leds, x, 10, mouthColor);
  // Slight thickness
  for (int x = 4; x <= 11; x++) setPixelSafe(leds, x, 11, mouthColor);
}

static void drawMouthFrown(CRGB leds[], const CRGB& mouthColor) {
  // A simple downward curve (upside-down smile)
  // y coordinates: 10-12 region, middle higher
  setPixelSafe(leds, 4, 11, mouthColor);
  setPixelSafe(leds, 5, 10, mouthColor);
  setPixelSafe(leds, 6, 9, mouthColor);
  setPixelSafe(leds, 7, 9, mouthColor);
  setPixelSafe(leds, 8, 9, mouthColor);
  setPixelSafe(leds, 9, 9, mouthColor);
  setPixelSafe(leds, 10, 10, mouthColor);
  setPixelSafe(leds, 11, 11, mouthColor);

}

void drawSmileyGreen(CRGB leds[]) {
  clearMatrix(leds);
  drawFaceOutline(leds, CHSV(96, 255, 220));   // green outline
  drawEyes(leds, CRGB::White);
  drawMouthSmile(leds, CRGB::White);
  FastLED.show();
}

void drawNeutralYellow(CRGB leds[]) {
  clearMatrix(leds);
  drawFaceOutline(leds, CHSV(40, 255, 240));   // yellow outline
  drawEyes(leds, CRGB::White);
  drawMouthNeutral(leds, CRGB::White);
  FastLED.show();
}

void drawFrownRed(CRGB leds[]) {
  clearMatrix(leds);
  drawFaceOutline(leds, CHSV(0, 255, 240));    // red outline
  drawEyes(leds, CRGB::White);
  drawMouthFrown(leds, CRGB::White);
  FastLED.show();
}


// ==================== LD2451 frame parser ====================
static const uint8_t HDR[4]  = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t TAIL[4] = {0xF8, 0xF7, 0xF6, 0xF5};

enum ParseState : uint8_t { FIND_HDR, READ_LEN, READ_PAYLOAD, READ_TAIL };
ParseState st = FIND_HDR;

uint8_t hdrPos = 0;
uint8_t tailPos = 0;

uint16_t payloadLen = 0;
uint16_t payloadRead = 0;
static const uint16_t PAYLOAD_MAX = 256;
uint8_t payload[PAYLOAD_MAX];

bool parseOneByte(uint8_t b) {
  //if (b < 0x10) Serial.print("0");
  //Serial.print(b, HEX);
  switch (st) {
    case FIND_HDR:
      if (b == HDR[hdrPos]) {
        hdrPos++;
        if (hdrPos == 4) { hdrPos = 0; st = READ_LEN; }
      } else hdrPos = 0;
      break;

    case READ_LEN: {
      static uint8_t lenBytes[2];
      static uint8_t lenPos = 0;
      lenBytes[lenPos++] = b;
      if (lenPos == 2) {
        lenPos = 0;
        payloadLen = (uint16_t)lenBytes[0] | ((uint16_t)lenBytes[1] << 8);
        if (payloadLen > PAYLOAD_MAX) { st = FIND_HDR; payloadRead = 0; return false; }
        payloadRead = 0;
        st = READ_PAYLOAD;
      }
      break;
    }

    case READ_PAYLOAD:
      payload[payloadRead++] = b;
      if (payloadRead >= payloadLen) { tailPos = 0; st = READ_TAIL; }
      break;

    case READ_TAIL:
      //Serial.println("TAIL");
      if (b == TAIL[tailPos]) {
        tailPos++;
        if (tailPos == 4) { st = FIND_HDR; tailPos = 0; return true; }
      } else { st = FIND_HDR; tailPos = 0; }
      break;
  }
  return false;
}

static const uint8_t BYTES_PER_TARGET = 5;
static const uint8_t PAYLOAD_HEADER_BYTES = 2;  // targetQuantity + alarmInformation

uint8_t parseRadarPayload(const uint8_t* payloadBuf,
                          uint16_t payloadLen,
                          RadarData* outTargets,
                          uint8_t maxTargets)
{
  if (!payloadBuf || !outTargets) return 0;
  if (payloadLen < PAYLOAD_HEADER_BYTES) return 0;

  uint8_t targetQty = payloadBuf[0];
  uint8_t alarmInfo = payloadBuf[1];

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
    // temp fake for testing
    rd.speed          = payloadBuf[base + 3];   // km/h
    rd.snr            = payloadBuf[base + 4];

    // Convert once and store
    rd.speedMph = rd.speed * 0.621371f;

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

bool payloadToSpeedMph(float &outMph) {
  if (payloadLen < 2) return false;

  const uint8_t MAX_TARGETS = 8;
  RadarData targets[MAX_TARGETS];

  uint8_t count = parseRadarPayload(payload, payloadLen, targets, MAX_TARGETS);
  if (count == 0) {
    outMph = 0.0f;
    return true; // valid frame, just no targets
  }

  // Current policy: show the fastest target
  float maxMph = 0.0f;
  for (uint8_t i = 0; i < count; i++) {
    if (targets[i].speedMph > maxMph) {
      maxMph = targets[i].speedMph;
    }
  }

  outMph = maxMph;
  return true;
}

/*
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

void trackTime(){
  currentMillis = millis();

  if (currentMillis - previousMillis >= interval) {
    // save the last time you updated the seconds
    previousMillis = currentMillis;
    
    // increment the seconds counter
    seconds++;
       
    // You can add other code here that you want to run every second
  }  
}

// ==================== Setup / Loop ====================
void setup() {
  Serial.begin(115200);
  delay(200);

  RadarSerialFront.begin(RADAR_BAUD);
  RadarSerialBack.begin(RADAR_BAUD);

  FastLED.addLeds<WS2812B, FRONT_LED_PIN, GRB>(frontLeds, NUM_LEDS);
  FastLED.addLeds<WS2812B, BACK_LED_PIN, GRB>(backLeds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  clearMatrix(frontLeds);
  clearMatrix(backLeds);
  FastLED.show();

  Serial.println("Mega2560: LD2451 -> MPH -> 2-digit 7seg on 16x16 (Serial1).");
  Serial.println("Wiring: LD2451 TX->Mega19(RX1), GND->GND, VCC->5V. (Optional: Mega18(TX1)->LD2451 RX via level shift).");
}

float frontMph = 0.0f;
float lastFrontMph = 0.0f;
float backMph = 0.0f;
float lastBackMph = 0.0f;
void loop() {
  trackTime();
  while (RadarSerialFront.available() > 0) {
    
    uint8_t b = (uint8_t)RadarSerialFront.read();
    if (parseOneByte(b)) {
      if (payloadToSpeedMph(frontMph)) {
        Serial.print("Speed Front: ");
        Serial.print(frontMph, 1);
        Serial.println(" mph");
      }
    }
  }
  while (RadarSerialBack.available() > 0) {
    
    uint8_t b = (uint8_t)RadarSerialBack.read();
    if (parseOneByte(b)) {
      if (payloadToSpeedMph(backMph)) {
        Serial.print("Speed Back: ");
        Serial.print(backMph, 1);
        Serial.println(" mph");
      }
    }
  }

  showSpeed(isFrontClear, frontLeds, frontMph, lastFrontMph);
  showSpeed(isBackClear, backLeds, backMph, lastBackMph);
}
