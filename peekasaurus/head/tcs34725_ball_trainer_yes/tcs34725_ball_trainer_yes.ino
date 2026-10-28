/*
  ============================================================================
  TCS34725  -  Ping-pong ball colour trainer & classifier  (+ NeoPixel strip)
  ============================================================================

  Ported from the TCS3200 version. The sensor is now I2C and returns 16-bit
  counts directly, so all the pulse-width timing is gone. Everything above
  that - white/black balance, chromaticity, teaching, nearest-centroid
  matching, the strip - works exactly as before.

  WIRING (Arduino Uno/Nano)
     VIN -> 5V         (board has its own regulator + level shifting)
     GND -> GND
     SDA -> A4         fixed by the hardware I2C peripheral
     SCL -> A5         fixed
     LED -> D3         pulled high on the module: HIGH/float = on, LOW = off
     3V3 -> leave unconnected. It is an OUTPUT, do not feed 5V into it.
     INT -> leave unconnected.

  NEOPIXEL STRIP
     DIN -> D2      through a 300-500 ohm resistor
     5V  -> 5V      a 1000uF cap across 5V/GND at the strip is advised
     GND -> GND     must share ground with the Arduino
     Keep the strip's light OUT of the sensor chamber.

  LIBRARIES (both via Library Manager)
     "Adafruit TCS34725"
     "Adafruit NeoPixel"

  SERIAL: 115200 baud, line ending "Newline" or "Both NL & CR".

  COMMANDS
     ?              help
     w              WHITE balance  (white reference in place)
     k              BLACK balance  (matte black card against the sensor)
     t <name>       TEACH: take a reading and add it to class <name>
     c              classify once
     m              live classify (send any line to stop)
     s              live raw stream (send any line to stop)
     l              list taught classes + calibration
     d <name>       delete a class
     x              erase everything
     h <value>      set match threshold (default 0.080, smaller = stricter)
     g <1|4|16|60>  set sensor gain - REDO w AND k AFTER CHANGING THIS
     p <name> <hex> set the strip colour for a class, e.g. p orange ff7a1a
     p <name> auto  go back to the measured colour for that class

  FIRST RUN: this sketch uses a new EEPROM signature, so calibration and
  training start empty. That is intentional - a different sensor has a
  different spectral response, so the old centroids would be meaningless.
  ============================================================================
*/

#include <EEPROM.h>
#include <Wire.h>
#include <avr/pgmspace.h>
#include <Adafruit_TCS34725.h>
#include <Adafruit_NeoPixel.h>

/* ========================= PIN MAP ========================= */
const int8_t  PIN_LED   = 3;     // -1 if you hard-wire the module LEDs on
const uint8_t PIN_STRIP = 2;

/* ========================= STRIP ========================= */
const uint16_t NUM_PIXELS = 64;
// POWER: 64 pixels at full white draw up to ~3.8A. Brightness scales that
// roughly linearly, so 40/255 puts a saturated colour near 0.3-0.5A - already
// more than a laptop USB port can give alongside the Nano. Use an external
// 5V supply of 2A or more, wired to the strip directly, with its ground tied
// to the Nano's. Raise this only once the strip has its own supply.
const uint8_t  STRIP_BRIGHTNESS = 40;
const uint16_t HOLD_MS          = 2000;
const bool     AUTO_LIVE        = true; // true = classify at power-up

/* ========================= DETECTION ========================= */
// Readings that must agree before a ball is reported. Each reading costs
// one integration (~24ms) plus I2C overhead, so this is the detection
// latency: 2 is about 60ms, 3 about 90ms. A ball falling past is only in
// view for roughly 60-100ms, so 3 often misses it entirely.
const uint8_t CONFIRM = 2;

/* ========================= TUNING ========================= */
// ONE integration time for everything. The sensor's maximum count scales
// with integration time (24ms -> 10240, 154ms -> 65535), so calibrating at
// one setting and reading at another would put every live reading on a
// different scale. Accuracy comes from averaging more samples instead.
#define INTEG  TCS34725_INTEGRATIONTIME_24MS
const uint8_t  SAMPLES_SLOW  = 4;     // averaged when accuracy matters
const uint8_t  SAMPLES_FAST  = 1;
const uint16_t LED_WARMUP_MS = 250;
const uint16_t SATURATED     = 9500;  // near the 10240 ceiling: gain too high
const uint16_t TOO_DIM       = 800;   // gain too low to use the range well

/* ========================= STORAGE ========================= */
#define MAX_CLASSES 8
#define NAME_LEN    12
const uint32_t MAGIC = 0x34725001UL;   // bumped: Calib layout changed

struct Calib {
  uint32_t magic;
  float    whiteCount[3];   // counts on the white reference
  float    blackCount[3];   // counts in the dark
  float    threshold;
  uint8_t  gainIdx;         // index into GAINS[]
};

struct ColorClass {
  bool     used;
  char     name[NAME_LEN];
  float    r, g, b;      // chromaticity centroid
  float    lum;          // normalised brightness centroid
  uint16_t n;
};

// Every struct must be declared above the first function definition: the
// Arduino IDE inserts generated prototypes there.
struct Reading {
  uint16_t raw[3];       // sensor counts, R G B
  uint16_t clear;        // unfiltered channel
  float    rgb[3];       // 0..255 white-balanced
  float    r, g, b;      // chromaticity, sums to 1
  float    lum;          // 0..1
};

struct Swatch { uint8_t r, g, b; uint8_t set; };
struct SwatchTable { uint32_t magic; Swatch c[MAX_CLASSES]; };

// Canonical colours, matched by class name. A class called "orange" shows a
// proper orange rather than the slightly muddy value the LEDs measured off a
// curved ball. Kept in flash (PROGMEM) so it costs no RAM - which matters
// with 64 pixels taking 192 bytes for their frame buffer.
// Precedence: an explicit 'p' colour, then this table, then the measurement.
struct NamedColour { char name[9]; uint8_t r, g, b; };
const NamedColour NAMED[] PROGMEM = {
  { "white",  255, 255, 255 },
  { "red",    230,  30,  40 },
  { "orange", 255, 120,  10 },
  { "yellow", 255, 210,  20 },
  { "green",   30, 190,  60 },
  { "blue",    20, 110, 240 },
  { "cyan",     0, 200, 215 },
  { "purple", 140,  70, 230 },
  { "violet", 140,  70, 230 },
  { "pink",   255, 120, 180 },
  { "brown",  150,  90,  45 },
  { "black",   20,  20,  20 },
};
const uint8_t NAMED_COUNT = sizeof(NAMED) / sizeof(NAMED[0]);

Calib      cal;
ColorClass classes[MAX_CLASSES];

const tcs34725Gain_t GAINS[4] = {
  TCS34725_GAIN_1X, TCS34725_GAIN_4X, TCS34725_GAIN_16X, TCS34725_GAIN_60X
};
const uint8_t GAIN_VALUES[4] = { 1, 4, 16, 60 };

Adafruit_TCS34725 tcs(INTEG, TCS34725_GAIN_4X);

uint8_t gSamples = SAMPLES_SLOW;
bool    ledHold  = false;

const int EE_CAL = 0;
const int EE_CLS = sizeof(Calib) + 8;

const int      EE_SW    = 400;
const uint32_t SW_MAGIC = 0x5357C002UL;   // bumped alongside MAGIC
static_assert(EE_CLS + MAX_CLASSES * sizeof(ColorClass) <= EE_SW,
              "class table overlaps swatch table - move EE_SW up");

SwatchTable sw;
Adafruit_NeoPixel strip(NUM_PIXELS, PIN_STRIP, NEO_GRB + NEO_KHZ800);

int8_t   stripClass   = -1;
bool     stripHolding = false;
uint32_t stripOffAt   = 0;

/* ========================= LOW LEVEL ========================= */

void ledOn(bool on) {
  if (PIN_LED < 0) return;
  if (!on && ledHold) return;
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

// Only the sample count changes between modes; integration time is fixed so
// every reading stays on the same scale as the calibration.
void setSpeed(bool fast) {
  gSamples = fast ? SAMPLES_FAST : SAMPLES_SLOW;
}

void readRaw(uint16_t out[3], uint16_t &clear) {
  ledOn(true);
  if (!ledHold) delay(LED_WARMUP_MS);

  uint32_t sum[3] = {0, 0, 0}, sumC = 0;
  for (uint8_t i = 0; i < gSamples; i++) {
    uint16_t r, g, b, c;
    tcs.getRawData(&r, &g, &b, &c);       // this call waits out the integration
    sum[0] += r; sum[1] += g; sum[2] += b; sumC += c;
  }
  for (uint8_t i = 0; i < 3; i++) out[i] = sum[i] / gSamples;
  clear = sumC / gSamples;

  ledOn(false);
}

// Counts are linear in light, so this is a plain two-point fit - much more
// direct than the reciprocal the frequency sensor needed.
float normChannel(float v, float w, float b) {
  if (w - b < 1.0f) return 0;
  float x = (v - b) / (w - b);
  if (x < 0)     x = 0;
  if (x > 1.5f)  x = 1.5f;
  return x * 255.0f;
}

void measure(Reading &m) {
  readRaw(m.raw, m.clear);
  for (uint8_t i = 0; i < 3; i++)
    m.rgb[i] = normChannel((float)m.raw[i], cal.whiteCount[i], cal.blackCount[i]);

  float sum = m.rgb[0] + m.rgb[1] + m.rgb[2];
  if (sum < 1.0f) { m.r = m.g = m.b = 1.0f / 3.0f; }
  else            { m.r = m.rgb[0] / sum; m.g = m.rgb[1] / sum; m.b = m.rgb[2] / sum; }
  m.lum = sum / (3.0f * 255.0f);
  if (m.lum > 1.5f) m.lum = 1.5f;
}

/* ========================= EEPROM ========================= */

void saveAll() {
  cal.magic = MAGIC;
  EEPROM.put(EE_CAL, cal);
  for (uint8_t i = 0; i < MAX_CLASSES; i++)
    EEPROM.put(EE_CLS + i * (int)sizeof(ColorClass), classes[i]);
}

void loadAll() {
  EEPROM.get(EE_CAL, cal);
  if (cal.magic != MAGIC) {
    cal.magic = MAGIC;
    for (uint8_t i = 0; i < 3; i++) { cal.whiteCount[i] = 1000; cal.blackCount[i] = 0; }
    cal.threshold = 0.080;
    cal.gainIdx   = 1;                      // 4x
    for (uint8_t i = 0; i < MAX_CLASSES; i++) { classes[i].used = false; classes[i].n = 0; }
    saveAll();
    Serial.println(F("! new EEPROM layout - run 'w' and 'k', then teach."));
    return;
  }
  if (cal.gainIdx > 3) cal.gainIdx = 1;
  for (uint8_t i = 0; i < MAX_CLASSES; i++)
    EEPROM.get(EE_CLS + i * (int)sizeof(ColorClass), classes[i]);
}

void saveSwatches() { sw.magic = SW_MAGIC; EEPROM.put(EE_SW, sw); }

void loadSwatches() {
  EEPROM.get(EE_SW, sw);
  if (sw.magic != SW_MAGIC) {
    for (uint8_t i = 0; i < MAX_CLASSES; i++) sw.c[i].set = 0;
    saveSwatches();
  }
}

/* ========================= STRIP ========================= */

// Returns true and fills r/g/b if the class name matches the NAMED table.
bool namedColour(const char *name, uint8_t &r, uint8_t &g, uint8_t &b) {
  NamedColour n;
  for (uint8_t i = 0; i < NAMED_COUNT; i++) {
    memcpy_P(&n, &NAMED[i], sizeof(n));
    if (strcasecmp(name, n.name) == 0) { r = n.r; g = n.g; b = n.b; return true; }
  }
  return false;
}

uint32_t classColour(int idx) {
  uint8_t r, g, b;
  if (sw.c[idx].set) {
    r = sw.c[idx].r; g = sw.c[idx].g; b = sw.c[idx].b;
  } else if (namedColour(classes[idx].name, r, g, b)) {
    // canonical colour for this name
  } else {
    float sum = classes[idx].lum * 765.0f;
    float fr = classes[idx].r * sum, fg = classes[idx].g * sum, fb = classes[idx].b * sum;
    float mx = max(fr, max(fg, fb));
    if (mx < 1) mx = 1;
    r = (uint8_t)min(255.0f, fr * 255.0f / mx);
    g = (uint8_t)min(255.0f, fg * 255.0f / mx);
    b = (uint8_t)min(255.0f, fb * 255.0f / mx);
  }
  return strip.gamma32(strip.Color(r, g, b));
}

void stripLight(int idx) {
  strip.fill(classColour(idx));
  strip.show();
  stripClass = idx;
  stripHolding = false;
}

void stripDark() {
  strip.clear();
  strip.show();
  stripClass = -1;
  stripHolding = false;
}

void stripRelease() {
  if (stripClass < 0 || stripHolding) return;
  stripHolding = true;
  stripOffAt = millis() + HOLD_MS;
}

void stripTick() {
  if (stripHolding && (int32_t)(millis() - stripOffAt) >= 0) stripDark();
}

/* ========================= CLASSES ========================= */

int findClass(const char *name) {
  for (uint8_t i = 0; i < MAX_CLASSES; i++)
    if (classes[i].used && strcasecmp(classes[i].name, name) == 0) return i;
  return -1;
}

int freeSlot() {
  for (uint8_t i = 0; i < MAX_CLASSES; i++) if (!classes[i].used) return i;
  return -1;
}

float distanceTo(const ColorClass &c, const Reading &m) {
  float dr = c.r - m.r, dg = c.g - m.g, db = c.b - m.b, dl = c.lum - m.lum;
  return sqrt(dr*dr + dg*dg + db*db + 0.15f * dl * dl);
}

void teach(const char *name, const Reading &m) {
  int idx = findClass(name);
  if (idx < 0) {
    idx = freeSlot();
    if (idx < 0) { Serial.println(F("! no free class slots - delete one first")); return; }
    classes[idx].used = true;
    strncpy(classes[idx].name, name, NAME_LEN - 1);
    classes[idx].name[NAME_LEN - 1] = 0;
    classes[idx].r = classes[idx].g = classes[idx].b = classes[idx].lum = 0;
    classes[idx].n = 0;
    sw.c[idx].set = 0;
    saveSwatches();
  }
  ColorClass &c = classes[idx];
  float n = c.n;
  c.r   = (c.r   * n + m.r)   / (n + 1);
  c.g   = (c.g   * n + m.g)   / (n + 1);
  c.b   = (c.b   * n + m.b)   / (n + 1);
  c.lum = (c.lum * n + m.lum) / (n + 1);
  c.n++;
  saveAll();

  Serial.print(F("taught '")); Serial.print(c.name);
  Serial.print(F("'  samples=")); Serial.print(c.n);
  Serial.print(F("  this reading dist from centroid="));
  Serial.println(distanceTo(c, m), 4);
}

/* ========================= PRINTING ========================= */

void printReading(const Reading &m) {
  Serial.print(F("raw  R="));    Serial.print(m.raw[0]);
  Serial.print(F(" G="));        Serial.print(m.raw[1]);
  Serial.print(F(" B="));        Serial.print(m.raw[2]);
  Serial.print(F(" C="));        Serial.print(m.clear);
  Serial.print(F("   RGB255 ")); Serial.print(m.rgb[0], 0);
  Serial.print(F(","));          Serial.print(m.rgb[1], 0);
  Serial.print(F(","));          Serial.print(m.rgb[2], 0);
  Serial.print(F("   chroma ")); Serial.print(m.r, 3);
  Serial.print(F(","));          Serial.print(m.g, 3);
  Serial.print(F(","));          Serial.print(m.b, 3);
  Serial.print(F("   lum "));    Serial.println(m.lum, 3);
}

int bestMatch(const Reading &m, float &bd, int &second, float &sd) {
  int best = -1;
  bd = 1e9; sd = 1e9; second = -1;
  for (uint8_t i = 0; i < MAX_CLASSES; i++) {
    if (!classes[i].used) continue;
    float d = distanceTo(classes[i], m);
    if (d < bd) { sd = bd; second = best; bd = d; best = i; }
    else if (d < sd) { sd = d; second = i; }
  }
  return best;
}

bool isEmptyClass(int idx) {
  return idx >= 0 && strcasecmp(classes[idx].name, "empty") == 0;
}

void printMatch(int best, float bd, int second, float sd) {
  if (bd <= cal.threshold) Serial.print(F(">> "));
  else                     Serial.print(F("?? UNKNOWN, closest "));
  Serial.print(classes[best].name);
  Serial.print(F("   d=")); Serial.print(bd, 4);
  if (second >= 0) {
    Serial.print(F("   2nd=")); Serial.print(classes[second].name);
    Serial.print(F(" d="));     Serial.print(sd, 4);
  }
  Serial.println();
}

void classify(const Reading &m, bool verbose) {
  float bd, sd; int second;
  int best = bestMatch(m, bd, second, sd);
  if (best < 0) { Serial.println(F("? nothing taught yet")); return; }
  printMatch(best, bd, second, sd);
  if (verbose) printReading(m);
}

void listAll() {
  Serial.println(F("--- calibration ---"));
  Serial.print(F("white counts  "));
  for (uint8_t i = 0; i < 3; i++) { Serial.print(cal.whiteCount[i], 1); Serial.print(' '); }
  Serial.println();
  Serial.print(F("black counts  "));
  for (uint8_t i = 0; i < 3; i++) { Serial.print(cal.blackCount[i], 1); Serial.print(' '); }
  Serial.println();
  Serial.print(F("threshold     ")); Serial.println(cal.threshold, 4);
  Serial.print(F("gain          ")); Serial.print(GAIN_VALUES[cal.gainIdx]); Serial.println(F("x"));

  Serial.println(F("--- classes (chroma r,g,b / lum / n) ---"));
  uint8_t count = 0;
  for (uint8_t i = 0; i < MAX_CLASSES; i++) {
    if (!classes[i].used) continue;
    count++;
    Serial.print(' '); Serial.print(classes[i].name);
    Serial.print(F("\t")); Serial.print(classes[i].r, 3);
    Serial.print(F(", "));  Serial.print(classes[i].g, 3);
    Serial.print(F(", "));  Serial.print(classes[i].b, 3);
    Serial.print(F("\tlum ")); Serial.print(classes[i].lum, 3);
    Serial.print(F("\tn=")); Serial.print(classes[i].n);
    Serial.print(F("\tcol "));
    if (sw.c[i].set) {
      char hex[8];
      snprintf(hex, sizeof(hex), "#%02x%02x%02x", sw.c[i].r, sw.c[i].g, sw.c[i].b);
      Serial.println(hex);
    } else {
      Serial.println(F("auto"));
    }
  }
  if (!count) Serial.println(F(" (none)"));

  Serial.println(F("--- pairwise separation (want >> threshold) ---"));
  for (uint8_t i = 0; i < MAX_CLASSES; i++) {
    if (!classes[i].used) continue;
    for (uint8_t j = i + 1; j < MAX_CLASSES; j++) {
      if (!classes[j].used) continue;
      float dr = classes[i].r - classes[j].r;
      float dg = classes[i].g - classes[j].g;
      float db = classes[i].b - classes[j].b;
      float dl = classes[i].lum - classes[j].lum;
      float d = sqrt(dr*dr + dg*dg + db*db + 0.15f*dl*dl);
      Serial.print(' '); Serial.print(classes[i].name);
      Serial.print(F(" <-> "));  Serial.print(classes[j].name);
      Serial.print(F("  "));     Serial.print(d, 4);
      if (d < 2 * cal.threshold) Serial.print(F("   <-- too close, may confuse"));
      Serial.println();
    }
  }
}

void help() {
  Serial.println(F("w=white bal  k=black bal  t <name>=teach  c=classify  m=live"));
  Serial.println(F("s=stream  l=list  d <name>=delete  x=erase all  h <v>=threshold"));
  Serial.println(F("g <1|4|16|60>=gain (redo w+k after)  p <name> <rrggbb|auto>=strip colour"));
}

/* ========================= COMMANDS ========================= */

bool serialLine(char *buf, uint8_t len) {
  static uint8_t pos = 0;
  static char work[40];
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      work[pos] = 0; pos = 0;
      strncpy(buf, work, len - 1); buf[len - 1] = 0;
      return true;
    }
    if (pos < sizeof(work) - 1) work[pos++] = ch;
  }
  return false;
}

void liveMode(bool doClassify) {
  setSpeed(true);
  ledHold = true;
  ledOn(true);
  delay(LED_WARMUP_MS);

  int     stable = -2, cand = -2;
  uint8_t agree = 0;
  uint16_t count = 0;

  if (doClassify) Serial.println(F("live classify - drop balls, send any line to stop"));
  else            Serial.println(F("live stream (fast) - send any line to stop"));

  while (true) {
    char dummy[40];
    if (serialLine(dummy, sizeof(dummy))) break;

    stripTick();

    uint32_t t0 = millis();
    Reading m; measure(m);
    uint32_t dt = millis() - t0;

    if (!doClassify) {
      Serial.print(dt); Serial.print(F("ms  "));
      printReading(m);
    }

    float bd, sd; int second;
    int best = bestMatch(m, bd, second, sd);
    if (best < 0) {
      if (doClassify) { Serial.println(F("? nothing taught yet")); break; }
      continue;
    }

    int state = (bd > cal.threshold || isEmptyClass(best)) ? -1 : best;
    if (state == cand) { if (agree < 255) agree++; }
    else               { cand = state; agree = 1; }

    if (agree >= CONFIRM && cand != stable) {
      stable = cand;
      if (stable >= 0) {
        count++;
        stripLight(stable);
        if (doClassify) {
          Serial.print(F("#")); Serial.print(count); Serial.print(F("  "));
          printMatch(best, bd, second, sd);
        }
      } else {
        stripRelease();
      }

      // Verdict line for the web page. The board is the single source of
      // truth: the page displays this rather than classifying again, so the
      // screen and the strip can never disagree on a marginal call.
      if (!doClassify) {
        Serial.print(F("@@ "));
        if (stable >= 0) { Serial.print(classes[stable].name); Serial.print(' '); Serial.println(bd, 4); }
        else             { Serial.println('-'); }
      }
    }
  }

  stripRelease();
  ledHold = false;
  ledOn(false);
  setSpeed(false);
  Serial.print(F("stopped - ")); Serial.print(count); Serial.println(F(" balls seen"));
}

void handle(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;

  char cmd = *line;
  char *arg = line + 1;
  while (*arg == ' ') arg++;

  switch (cmd) {
    case '?': help(); break;

    case 'w': {
      Serial.println(F("white balance: hold the WHITE reference in place..."));
      delay(800);
      setSpeed(false);
      uint16_t raw[3], c;
      readRaw(raw, c);
      for (uint8_t i = 0; i < 3; i++) cal.whiteCount[i] = raw[i];
      saveAll();
      Serial.print(F("white counts R/G/B: "));
      Serial.print(raw[0]); Serial.print(' ');
      Serial.print(raw[1]); Serial.print(' ');
      Serial.println(raw[2]);
      if (raw[0] > SATURATED || raw[1] > SATURATED || raw[2] > SATURATED)
        Serial.println(F("! saturated - lower the gain with 'g' and redo w and k"));
      else if (raw[0] < TOO_DIM && raw[1] < TOO_DIM && raw[2] < TOO_DIM)
        Serial.println(F("! very dim - raise the gain with 'g' and redo w and k"));
      break;
    }

    case 'k': {
      Serial.println(F("black balance: matte black card against the sensor..."));
      delay(800);
      setSpeed(false);
      uint16_t raw[3], c;
      readRaw(raw, c);
      for (uint8_t i = 0; i < 3; i++) cal.blackCount[i] = raw[i];
      saveAll();
      Serial.print(F("black counts R/G/B: "));
      Serial.print(raw[0]); Serial.print(' ');
      Serial.print(raw[1]); Serial.print(' ');
      Serial.println(raw[2]);
      break;
    }

    case 't': {
      if (!*arg) { Serial.println(F("usage: t <name>")); break; }
      setSpeed(false);
      Reading m; measure(m);
      printReading(m);
      teach(arg, m);
      break;
    }

    case 'c': { setSpeed(false); Reading m; measure(m); classify(m, true); break; }
    case 'm': liveMode(true);  break;
    case 's': liveMode(false); break;
    case 'l': listAll(); break;

    case 'd': {
      int idx = findClass(arg);
      if (idx < 0) { Serial.println(F("not found")); break; }
      classes[idx].used = false;
      classes[idx].n = 0;
      sw.c[idx].set = 0;
      saveAll();
      saveSwatches();
      if (stripClass == idx) stripDark();
      Serial.println(F("deleted"));
      break;
    }

    case 'x': {
      for (uint8_t i = 0; i < MAX_CLASSES; i++) { classes[i].used = false; classes[i].n = 0; sw.c[i].set = 0; }
      cal.threshold = 0.080;
      saveAll();
      saveSwatches();
      stripDark();
      Serial.println(F("erased - redo white/black balance"));
      break;
    }

    case 'h': {
      float v = atof(arg);
      if (v > 0) { cal.threshold = v; saveAll(); }
      Serial.print(F("threshold = ")); Serial.println(cal.threshold, 4);
      break;
    }

    case 'g': {
      int v = atoi(arg);
      int8_t idx = -1;
      for (uint8_t i = 0; i < 4; i++) if (GAIN_VALUES[i] == v) idx = i;
      if (idx < 0) { Serial.println(F("usage: g <1|4|16|60>")); break; }
      cal.gainIdx = idx;
      tcs.setGain(GAINS[idx]);
      saveAll();
      Serial.print(F("gain = ")); Serial.print(GAIN_VALUES[idx]);
      Serial.println(F("x  - calibration is now stale, redo w and k"));
      break;
    }

    case 'p': {
      char *space = strchr(arg, ' ');
      if (!*arg || !space) { Serial.println(F("usage: p <name> <rrggbb|auto>")); break; }
      *space = 0;
      char *val = space + 1;
      while (*val == ' ') val++;

      int idx = findClass(arg);
      if (idx < 0) { Serial.println(F("not found")); break; }

      if (strcasecmp(val, "auto") == 0) {
        sw.c[idx].set = 0;
      } else {
        if (*val == '#') val++;
        if (strlen(val) != 6) { Serial.println(F("colour must be 6 hex digits")); break; }
        uint32_t v = strtoul(val, NULL, 16);
        sw.c[idx].r = (v >> 16) & 0xFF;
        sw.c[idx].g = (v >> 8)  & 0xFF;
        sw.c[idx].b =  v        & 0xFF;
        sw.c[idx].set = 1;
      }
      saveSwatches();
      if (stripClass == idx) { strip.fill(classColour(idx)); strip.show(); }

      Serial.print(F("colour ")); Serial.print(classes[idx].name); Serial.print(F(" = "));
      if (sw.c[idx].set) {
        char hex[8];
        snprintf(hex, sizeof(hex), "#%02x%02x%02x", sw.c[idx].r, sw.c[idx].g, sw.c[idx].b);
        Serial.println(hex);
      } else {
        Serial.println(F("auto"));
      }
      break;
    }

    default: Serial.println(F("unknown command, send ? for help"));
  }
}

/* ========================= SETUP / LOOP ========================= */

void setup() {
  if (PIN_LED >= 0) { pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW); }

  Serial.begin(115200);
  while (!Serial) { ; }
  delay(200);

  strip.begin();
  strip.setBrightness(STRIP_BRIGHTNESS);
  strip.clear();
  strip.show();

  Serial.println(F("\nTCS34725 ball colour trainer"));

  if (!tcs.begin()) {
    Serial.println(F("! TCS34725 not found - check SDA=A4, SCL=A5, VIN=5V, GND"));
    while (true) { delay(1000); }
  }

  loadAll();
  loadSwatches();
  tcs.setGain(GAINS[cal.gainIdx]);
  setSpeed(false);
  help();

  if (AUTO_LIVE) liveMode(true);
}

void loop() {
  stripTick();
  char line[40];
  if (serialLine(line, sizeof(line))) handle(line);
}
