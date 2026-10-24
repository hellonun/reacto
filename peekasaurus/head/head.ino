/*
  ============================================================================
  Peekasaurus HEAD  -  ball colour trainer & classifier  (+ NeoPixel strip)
  ============================================================================

  A ball drops into the head, a TCS34725 reads its colour, and the NeoPixel
  strip lights up in that colour. Colours are taught, not hard-coded: you
  show the sensor each ball and it learns what that ball looks like under
  this particular lighting and geometry.

  See WIRING.md in this folder for the full build and training guide.

  ----------------------------------------------------------------------------
  RUNNING WITH OR WITHOUT A COMPUTER  ->  the AUTO_LIVE switch below
  ----------------------------------------------------------------------------
    AUTO_LIVE = false   needs colour-bench.html (or a Serial Monitor) to
                        start it. Use while setting up and training.
    AUTO_LIVE = true    classifies by itself from power-up. The strip works
                        with nothing plugged in. Use for the finished build.
  Train first with false, then set true and re-upload. Training survives the
  re-upload - it lives in EEPROM, not in the sketch.
  ----------------------------------------------------------------------------

  WIRING (Arduino Uno/Nano)
     VIN -> 5V         (board has its own regulator + level shifting)
     GND -> GND
     SDA -> A4         fixed by the hardware I2C peripheral
     SCL -> A5         fixed
     LED -> D3         pulled high on the module: HIGH/float = on, LOW = off
     3V3 -> leave unconnected. It is an OUTPUT, do not feed 5V into it.
     INT -> leave unconnected.

  BREAK BEAM (optional but recommended - set PIN_BEAM = -1 if not fitted)
     Receiver OUT -> D4
     VCC          -> 5V       (emitter too)
     GND          -> GND
     Mount the beam so it crosses the ball's path AT or JUST ABOVE the
     sensor window. Send 'b' to check wiring and polarity.

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
const int8_t  PIN_BEAM  = 4;     // break-beam receiver. -1 = no beam fitted

/* ========================= BREAK BEAM ========================= */
// With a beam fitted, the head stops guessing from colour alone: the break
// says exactly when a ball is in front of the sensor, so one trustworthy
// reading replaces the CONFIRM debounce. Set PIN_BEAM to -1 to go back to
// free-running detection.
const bool     BEAM_AUTO_POLARITY = true;  // learn "clear" from the resting
                                        // state when live mode starts. Keep
                                        // the chamber EMPTY at that moment.
const bool     BEAM_ACTIVE_LOW = true;  // only used if AUTO_POLARITY is false
// Break -> read delay. The beam catches the ball's LEADING EDGE; its body
// needs a few more ms to swing into the sensor's view. Measured with 'f' on
// this rig: chamber still dark at t=23, ball peaks at t=34, gone by t=64.
// 22 puts the 24ms read window over [22,46] - squarely on the pass.
// Re-run 'f' if the drop height or the beam position changes.
const uint16_t BEAM_SETTLE_MS  = 22;
const uint8_t  BEAM_FLUSH      = 1;     // stale integrations to discard (below)
// Ignore re-triggers. The 'f' trace shows this ball bouncing back through
// the sensor at t=120-196 and again at t=202-278, so a 250ms lockout would
// let the third pass count as a second ball. 400 covers the whole sequence.
const uint16_t BEAM_LOCKOUT_MS = 400;
// An unwired or dead beam looks exactly like a beam that never sees a ball:
// nothing triggers and the head goes quiet. If no edge at all arrives within
// this long, say so and fall back to free-running detection for the rest of
// the session, so the head keeps working. 0 disables the fallback.
const uint16_t BEAM_FALLBACK_MS = 8000;

/* ========================= STRIP ========================= */
const uint16_t NUM_PIXELS = 64;
// POWER: 64 pixels at full white draw up to ~3.8A. Brightness scales that
// roughly linearly, so 40/255 puts a saturated colour near 0.3-0.5A - already
// more than a laptop USB port can give alongside the Nano. Use an external
// 5V supply of 2A or more, wired to the strip directly, with its ground tied
// to the Nano's. Raise this only once the strip has its own supply.
const uint8_t  STRIP_BRIGHTNESS = 40;
const uint16_t HOLD_MS          = 2000;

// ===========================================================================
// AUTO_LIVE  -  the one switch that decides whether you need a computer
// ===========================================================================
//   false  The sketch waits for a command. colour-bench.html (or a Serial
//          Monitor) has to start it. The strip stays dark until then.
//          USE THIS WHILE CALIBRATING AND TRAINING.
//
//   true   The sketch starts classifying the moment it powers up. The strip
//          works standalone - no laptop, no browser, just USB power.
//          USE THIS FOR THE FINISHED BUILD.
//
// The page still works with true: connecting sends a newline, which drops
// the sketch back to its command prompt so you can train or re-calibrate.
// But see WIRING.md - after that, a byte of serial noise leaves it parked at
// the prompt with the strip dark until it is power-cycled.
// ===========================================================================
const bool     AUTO_LIVE        = true;

/* ========================= DETECTION ========================= */
// ONLY used when no beam is fitted (PIN_BEAM = -1, or the beam fallback
// fired). A beam makes this irrelevant: the break already proves a ball is
// there, so no agreement is needed.
//
// Readings that must agree before a ball is reported. Each costs one
// integration (~24ms) plus I2C overhead, so this IS the detection latency:
// 1 is ~30ms, 2 is ~60ms, 3 is ~90ms. A ball falling past is in view for
// only 60-100ms, so anything above 1 misses balls on this rig - measured,
// not guessed. The cost of 1 is that a half-in-frame reading can be
// reported, which the beam exists to eliminate.
const uint8_t CONFIRM = 1;

// How much BRIGHTNESS counts against hue when matching. Brightness varies a
// lot on a falling ball - the 'f' trace had the same ball at C=65, C=40 and
// C=165 on three passes - while its chromaticity barely moved. Lower this
// toward 0 if balls read correct-but-dim and still miss; the cost is that
// white, grey and black become indistinguishable, since they differ only in
// brightness. 0 is right if every ball is a saturated colour.
// 0 = match on hue alone. Set from measured data: in-flight readings of the
// SAME ball ranged C=65 to C=165 on three passes while its chromaticity
// moved less than 0.04, so brightness here is noise, not signal. At 0.15 a
// hue-perfect read whose brightness differed by 0.3 from training already
// scored 0.116 - more than the whole threshold - which is why correct
// colours kept coming back UNKNOWN.
// Raise it above 0 only if you add white, grey or black balls, which differ
// from each other ONLY in brightness.
const float LUM_WEIGHT = 0.0;

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
// Stream a reading now and then for the page's Training readout. Each one
// blocks beam polling for a whole integration, so it is OFF for live
// detection and ON only while the page is actually showing that readout.
bool    gIdleReads = true;
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
  return sqrt(dr*dr + dg*dg + db*db + LUM_WEIGHT * dl * dl);
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

/* ========================= BEAM ========================= */

// Whichever level the pin sits at with the chamber empty IS "clear", so the
// polarity of the receiver stops mattering. Learned when live mode starts
// and when 'b' runs. Set BEAM_AUTO_POLARITY false to force it by hand.
uint8_t beamClearLevel = HIGH;

void beamLearn() {
  if (PIN_BEAM < 0) return;
  if (BEAM_AUTO_POLARITY) beamClearLevel = digitalRead(PIN_BEAM);
  else                    beamClearLevel = BEAM_ACTIVE_LOW ? HIGH : LOW;
}

bool beamBroken() {
  if (PIN_BEAM < 0) return false;
  return digitalRead(PIN_BEAM) != beamClearLevel;
}

// getRawData hands back the integration that ALREADY finished, so the first
// read after a ball arrives still describes the empty chamber. Throwing that
// one away is what makes a beam-triggered reading trustworthy rather than
// merely fast.
void flushIntegrations(uint8_t n) {
  uint16_t r, g, b, c;
  for (uint8_t i = 0; i < n; i++) tcs.getRawData(&r, &g, &b, &c);
}

// The board's decision, for the web page to display.
void sendVerdict(int idx, float d) {
  Serial.print(F("@@ "));
  if (idx >= 0) { Serial.print(classes[idx].name); Serial.print(' '); Serial.println(d, 4); }
  else          { Serial.println('-'); }
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
      float d = sqrt(dr*dr + dg*dg + db*db + LUM_WEIGHT*dl*dl);
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
  Serial.println(F("v=verdicts only (precise beam timing)  b=beam monitor"));
  Serial.println(F("n=strip test  f=fall profile (sets BEAM_SETTLE_MS)"));
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

// Beam-triggered detection. The break says a ball is there, so there is
// nothing to debounce and nothing to infer from the colour: one reading per
// ball, taken at the right moment. Latency is the beam edge (microseconds)
// plus the flush and the read, instead of CONFIRM whole readings.
uint16_t liveModeBeam(bool doClassify, bool &fellBack) {
  uint16_t count = 0;
  bool     lit    = false;
  bool     armed  = !beamBroken();
  uint32_t lockoutUntil = 0;
  uint32_t nextIdleRead = 0;

  bool     lastBeam  = beamBroken();
  bool     edgeSeen  = false;
  uint32_t startedAt = millis();

  while (true) {
    char dummy[40];
    if (serialLine(dummy, sizeof(dummy))) break;
    stripTick();

    bool broken = beamBroken();

    if (broken != lastBeam) { lastBeam = broken; edgeSeen = true; }

    // Nothing from the beam at all: treat it as not fitted rather than
    // leaving the head mute.
    if (!edgeSeen && BEAM_FALLBACK_MS &&
        (int32_t)(millis() - startedAt) > (int32_t)BEAM_FALLBACK_MS) {
      Serial.println(F("! no beam activity - check D4 and the emitter ('b' to test)."));
      Serial.println(F("! falling back to free-running detection for this session."));
      fellBack = true;
      return count;
    }

    // --- ball arriving -----------------------------------------------
    if (broken && armed && (int32_t)(millis() - lockoutUntil) >= 0) {
      armed = false;
      lockoutUntil = millis() + BEAM_LOCKOUT_MS;

      if (BEAM_SETTLE_MS) delay(BEAM_SETTLE_MS);
      flushIntegrations(BEAM_FLUSH);

      Reading m; measure(m);
      float bd, sd; int second;
      int best = bestMatch(m, bd, second, sd);
      if (best < 0) {
        if (doClassify) { Serial.println(F("? nothing taught yet")); break; }
        continue;
      }

      if (bd <= cal.threshold && !isEmptyClass(best)) {
        count++;
        lit = true;
        stripLight(best);
        if (doClassify) {
          Serial.print(F("#")); Serial.print(count); Serial.print(F("  "));
          printMatch(best, bd, second, sd);
        } else {
          sendVerdict(best, bd);
        }
      } else if (doClassify) {
        Serial.print(F("   ")); printMatch(best, bd, second, sd);
      }
      continue;
    }

    // --- chamber clear -------------------------------------------------
    if (!broken) {
      if (lit) {
        lit = false;
        stripRelease();
        if (!doClassify) sendVerdict(-1, 0);
      }
      armed = true;

      // Keep the page's Training readout alive while nothing is dropping.
      // Skipped while a ball is in the beam so the trigger stays responsive.
      if (!doClassify && gIdleReads && (int32_t)(millis() - nextIdleRead) >= 0) {
        Reading m; measure(m);
        printReading(m);
        nextIdleRead = millis() + 120;
      }
    }
  }
  return count;
}

// Free-running detection, used when no beam is fitted: read continuously and
// debounce on CONFIRM agreeing readings.
uint16_t liveModeFree(bool doClassify) {
  int     stable = -2, cand = -2;
  uint8_t agree = 0;
  uint16_t count = 0;

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

      // The board is the single source of truth: the page displays this
      // rather than classifying again, so the screen and the strip can never
      // disagree on a marginal call.
      if (!doClassify) sendVerdict(stable, bd);
    }
  }
  return count;
}

void liveMode(bool doClassify) {
  setSpeed(true);
  ledHold = true;
  ledOn(true);
  delay(LED_WARMUP_MS);

  if (doClassify) Serial.print(F("live classify"));
  else            Serial.print(F("live stream (fast)"));
  if (PIN_BEAM >= 0) {
    Serial.print(F(" [beam on D")); Serial.print(PIN_BEAM);
    Serial.print(F(", clear=")); Serial.print(beamClearLevel == HIGH ? F("HIGH") : F("LOW"));
    Serial.print(F("]"));
  }
  Serial.println(F(" - drop balls, send any line to stop"));

  uint16_t count = 0;
  if (PIN_BEAM >= 0) {
    bool fellBack = false;
    count = liveModeBeam(doClassify, fellBack);
    if (fellBack) count += liveModeFree(doClassify);
  } else {
    count = liveModeFree(doClassify);
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

    case 'f': {
      // Measures how long after the beam breaks the ball actually reaches
      // the sensor window - the number BEAM_SETTLE_MS needs to be when the
      // beam sits above the window.
      if (PIN_BEAM < 0) { Serial.println(F("needs a beam (PIN_BEAM = -1)")); break; }
      Serial.println(F("fall profile - drop ONE ball, or send a line to cancel"));

      ledHold = true; ledOn(true); delay(LED_WARMUP_MS);
      beamLearn();

      // 2.4ms integration: the counts land on a different scale and are
      // noisy, but we only care WHEN the colour changes, not what it is.
      tcs.setIntegrationTime(TCS34725_INTEGRATIONTIME_2_4MS);
      { uint16_t a, b2, c2, d2; tcs.getRawData(&a, &b2, &c2, &d2); }

      bool cancelled = false;
      while (!beamBroken()) {
        char dummy[40];
        if (serialLine(dummy, sizeof(dummy))) { cancelled = true; break; }
      }

      if (!cancelled) {
        uint32_t t0 = millis();
        Serial.println(F("t(ms)\tR\tG\tB\tC"));
        for (uint8_t i = 0; i < 48; i++) {
          uint16_t r, g, b2, c2;
          tcs.getRawData(&r, &g, &b2, &c2);
          Serial.print(millis() - t0); Serial.print('\t');
          Serial.print(r);  Serial.print('\t');
          Serial.print(g);  Serial.print('\t');
          Serial.print(b2); Serial.print('\t');
          Serial.println(c2);
        }
        Serial.println(F("Find the row where C jumps and R/G/B separate."));
        Serial.println(F("That t is your BEAM_SETTLE_MS. Run it a few times."));
      }

      tcs.setIntegrationTime(INTEG);
      { uint16_t a, b2, c2, d2; tcs.getRawData(&a, &b2, &c2, &d2); }
      ledHold = false; ledOn(false);
      break;
    }

    case 'n': {
      Serial.print(F("strip test on D")); Serial.print(PIN_STRIP);
      Serial.print(F(", ")); Serial.print(NUM_PIXELS);
      Serial.println(F(" pixels: red, green, blue, white, off"));
      const uint32_t test[4] = { 0xFF0000UL, 0x00FF00UL, 0x0000FFUL, 0xFFFFFFUL };
      for (uint8_t i = 0; i < 4; i++) {
        strip.fill(strip.gamma32(test[i]));
        strip.show();
        delay(600);
      }
      strip.clear(); strip.show();
      Serial.println(F("done. Nothing lit? check NUM_PIXELS, DIN on D2, the strip's"));
      Serial.println(F("own 5V supply, and that its GND is tied to the Arduino's."));
      break;
    }

    case 'b': {
      if (PIN_BEAM < 0) { Serial.println(F("no beam fitted (PIN_BEAM = -1)")); break; }
      Serial.println(F("beam monitor - keep the chamber EMPTY, then block it by hand"));
      beamLearn();
      Serial.print(F("resting level taken as clear: "));
      Serial.println(beamClearLevel == HIGH ? F("HIGH") : F("LOW"));
      Serial.println(F("send any line to stop"));
      bool last = beamBroken();
      uint16_t edges = 0;
      while (true) {
        char dummy[40];
        if (serialLine(dummy, sizeof(dummy))) break;
        bool now = beamBroken();
        if (now != last) {
          last = now;
          edges++;
          Serial.print(millis()); Serial.print(F("ms  "));
          Serial.println(now ? F("BROKEN") : F("clear"));
        }
      }
      Serial.print(F("stopped - ")); Serial.print(edges); Serial.println(F(" edges"));
      if (!edges) Serial.println(F("No edges at all: check the receiver OUT is on D4 and the emitter is powered."));
      break;
    }

    case 'c': { setSpeed(false); Reading m; measure(m); classify(m, true); break; }
    case 'm': gIdleReads = false; liveMode(true);  break;
    case 's': gIdleReads = true;  liveMode(false); break;  // + Training readout
    case 'v': gIdleReads = false; liveMode(false); break;  // verdicts only
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
  // Pull-up suits an open-collector receiver and is harmless on a push-pull
  // one; without it a disconnected input floats and triggers on noise.
  if (PIN_BEAM >= 0) { pinMode(PIN_BEAM, INPUT_PULLUP); delay(5); beamLearn(); }

  Serial.begin(115200);
  while (!Serial) { ; }
  delay(200);

  strip.begin();
  strip.setBrightness(STRIP_BRIGHTNESS);
  strip.clear();
  strip.show();

  Serial.println(F("\nPeekasaurus head - ball colour trainer"));

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
