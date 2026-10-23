/*
  2x SW-18010P shake sensors -> prints "S1 shook" / "S2 shook"
  -------------------------------------------------------------
  Wiring (Arduino Mega 2560):
    Sensor 1 DO  -> D12
    Sensor 2 DO  -> D11
    Both VCC     -> 5V   (share via breadboard rail if needed)
    Both GND     -> GND  (use a common ground with motor supply)
    D13          -> on-board LED flashes on any shake (nothing to wire)
    Motors IA    -> D2,D3,D4,D5,D6,D7,D8,D9
    Motors IB    -> D22,D23,D53,D25,D26,D27,D28,D29  (motor 3 IB moved D24 -> D53)
    See WIRING.md for the full wiring guide.

  Detection: a "shake" = at least THRESHOLD signal edges within WINDOW ms,
  then ignore further shakes for COOLDOWN ms. Each sensor has its own
  settings and its own calibration.

  Serial monitor: 115200 baud, line ending "Newline". Type  h  for commands.
*/

#include <EEPROM.h>

const uint8_t PIN_LED = 13;
const uint8_t NUM_SENSORS = 2;
const uint8_t SIGNAL_PINS[NUM_SENSORS] = { 12, 11 };

// ---------- settings (saved to EEPROM, one block per sensor) ----------
struct Settings {
  uint16_t magic;
  uint16_t threshold;   // edges needed inside the window
  uint16_t windowMs;    // time window for counting edges
  uint16_t cooldownMs;  // dead time after a shake
  uint16_t debounceUs;  // ignore edges closer together than this
};

const uint16_t MAGIC = 0x5318;
const Settings DEFAULTS = { MAGIC, 4, 250, 800, 200 };
const uint8_t  BUF = 32;           // max threshold

// ---------- per-sensor state ----------
struct Sensor {
  uint8_t  pin;
  Settings cfg;
  unsigned long edgeTimes[BUF];
  uint8_t  head;
  uint8_t  stored;
  bool     lastLevel;
  unsigned long lastEdgeUs;
  unsigned long lastShakeMs;
  bool     shakenOnce;
  uint32_t shakeCount;
  uint16_t monitorEdges;
};

Sensor sensors[NUM_SENSORS];
uint8_t sel = 0;                   // currently selected sensor (0-based)

unsigned long ledOffAt = 0;
bool     monitorOn = false;
unsigned long monitorLastMs = 0;

char     lineBuf[32];
uint8_t  lineLen = 0;

void clearHistory(Sensor &s);

// ================================================================
// 8 motors on 4 L9110S modules. ORIGINAL Mega wiring (no rewiring).
// Motors 1-4 respond to shake sensor 1, motors 5-8 to shake sensor 2.
const uint8_t MOTOR_IA[8] = {2, 3, 4, 5, 6, 7, 8, 9};
const uint8_t MOTOR_IB[8] = {22, 23, 53, 25, 26, 27, 28, 29}; // change pin for motor 3

const unsigned long WAVE_TIME_MS = 2000;
const unsigned long MOTOR_STAGGER_MS = 300;
// Last motor starts at 900 ms, and ends exactly at 2000 ms.
const unsigned long MOTOR_CYCLE_MS = WAVE_TIME_MS - 3 * MOTOR_STAGGER_MS;
const uint8_t MAX_MOTOR_PWM = 200;

bool waveActive[2] = {false, false};
unsigned long waveStartedMs[2] = {0, 0};

void stopMotor(uint8_t motor) {
  digitalWrite(MOTOR_IA[motor], LOW);
  digitalWrite(MOTOR_IB[motor], LOW);
}

void driveMotor(uint8_t motor, int signedSpeed) {
  signedSpeed = constrain(signedSpeed, -255, 255);
  if (signedSpeed > 0) {
    digitalWrite(MOTOR_IB[motor], LOW);
    analogWrite(MOTOR_IA[motor], signedSpeed);
  } else if (signedSpeed < 0) {
    // Reverse with inverted PWM on the same hardware PWM pin.
    // IB is not PWM-capable on original wiring.
    digitalWrite(MOTOR_IB[motor], HIGH);
    analogWrite(MOTOR_IA[motor], 255 + signedSpeed);
  } else {
    stopMotor(motor);
  }
}

void startWave(uint8_t group, unsigned long now) {
  if (waveActive[group]) return;
  waveActive[group] = true;
  waveStartedMs[group] = now;
  Serial.print(F("Starting motor wave "));
  Serial.println(group + 1);
}

void updateWaves(unsigned long now) {
  for (uint8_t group = 0; group < 2; group++) {
    if (!waveActive[group]) continue;

    unsigned long elapsed = now - waveStartedMs[group];
    if (elapsed >= WAVE_TIME_MS) {
      for (uint8_t j = 0; j < 4; j++) stopMotor(group * 4 + j);
      waveActive[group] = false;
      clearHistory(sensors[group]);
      sensors[group].lastShakeMs = now;
      sensors[group].shakenOnce = true;
      continue;
    }

    for (uint8_t j = 0; j < 4; j++) {
      uint8_t motor = group * 4 + j;
      unsigned long beginAt = j * MOTOR_STAGGER_MS;
      if (elapsed < beginAt || elapsed >= beginAt + MOTOR_CYCLE_MS) {
        stopMotor(motor);
        continue;
      }

      float phase = float(elapsed - beginAt) / float(MOTOR_CYCLE_MS);
      // Each motor does one forward/back cycle, eased within each half.
      // phase: 0 -> .5 is forward, .5 -> 1 is reverse.
      float halfProgress = (phase < 0.5f) ? phase * 2.0f : (phase - 0.5f) * 2.0f;
      int pwm = (int)(MAX_MOTOR_PWM * sin(PI * halfProgress) + 0.5f);
      driveMotor(motor, phase < 0.5f ? pwm : -pwm);
    }
  }
}

// ================================================================
bool pollEdge(Sensor &s) {
  bool lvl = digitalRead(s.pin);
  if (lvl == s.lastLevel) return false;
  s.lastLevel = lvl;
  unsigned long nowUs = micros();
  if (nowUs - s.lastEdgeUs < s.cfg.debounceUs) return false;  // bounce, ignore
  s.lastEdgeUs = nowUs;
  return true;
}

void clearHistory(Sensor &s) {
  s.head = 0;
  s.stored = 0;
}

void onEdge(uint8_t i, unsigned long now) {
  Sensor &s = sensors[i];
  s.monitorEdges++;
  if (waveActive[i]) return; // Ignore retriggers during this group's wave.

  s.edgeTimes[s.head] = now;
  s.head = (s.head + 1) % BUF;
  if (s.stored < BUF) s.stored++;

  if (s.shakenOnce && (now - s.lastShakeMs < s.cfg.cooldownMs)) return;

  if (s.stored >= s.cfg.threshold) {
    uint8_t idx = (s.head + BUF - s.cfg.threshold) % BUF;  // oldest of last <threshold>
    if (now - s.edgeTimes[idx] <= s.cfg.windowMs) {
      s.shakeCount++;
      Serial.print(F("sensor ")); Serial.print(i + 1);
      Serial.print(F(" shook ")); Serial.println(s.shakeCount);
      s.lastShakeMs = now;
      s.shakenOnce = true;
      digitalWrite(PIN_LED, HIGH);
      ledOffAt = now + 150;
      clearHistory(s);
      startWave(i, now);
    }
  }
}

// ================================================================
// Calibration (one sensor at a time)
uint16_t measurePeak(Sensor &s, uint16_t durationMs, uint16_t &total) {
  const uint8_t MAXREC = 150;
  uint16_t t[MAXREC];
  uint8_t n = 0;
  total = 0;

  s.lastLevel = digitalRead(s.pin);
  unsigned long start = millis();
  while (millis() - start < durationMs) {
    if (pollEdge(s)) {
      total++;
      if (n < MAXREC) t[n++] = (uint16_t)(millis() - start);
    }
  }

  uint16_t best = 0;
  uint8_t j = 0;
  for (uint8_t k = 0; k < n; k++) {
    while (t[k] - t[j] > s.cfg.windowMs) j++;
    uint16_t c = k - j + 1;
    if (c > best) best = c;
  }
  return best;
}

void countdown(const __FlashStringHelper *msg) {
  Serial.println(msg);
  for (int i = 3; i > 0; i--) {
    Serial.print(F("  ")); Serial.print(i); Serial.println(F("..."));
    delay(1000);
  }
  Serial.println(F("  GO"));
}

void calibrate(uint8_t i) {
  Sensor &s = sensors[i];
  Serial.println();
  Serial.print(F("=== CALIBRATION: sensor ")); Serial.print(i + 1);
  Serial.print(F(" (D")); Serial.print(s.pin); Serial.println(F(") ==="));
  Serial.print(F("Signal idles "));
  Serial.println(digitalRead(s.pin) ? F("HIGH") : F("LOW"));
  Serial.print(F("Using window = ")); Serial.print(s.cfg.windowMs); Serial.println(F(" ms"));

  uint16_t stillTotal, shakeTotal;

  countdown(F("Step 1/2: keep this sensor STILL."));
  uint16_t noise = measurePeak(s, 3000, stillTotal);
  Serial.print(F("  still: peak ")); Serial.print(noise);
  Serial.print(F(" edges/window, total ")); Serial.println(stillTotal);

  countdown(F("Step 2/2: give THIS sensor the LIGHTEST shake you want detected, keep shaking."));
  uint16_t shake = measurePeak(s, 4000, shakeTotal);
  Serial.print(F("  shake: peak ")); Serial.print(shake);
  Serial.print(F(" edges/window, total ")); Serial.println(shakeTotal);

  if (shakeTotal == 0) {
    Serial.print(F("!! No signal while shaking. Check DO -> D")); Serial.print(s.pin);
    Serial.println(F(", VCC, GND,"));
    Serial.println(F("   and turn the module's potentiometer; try 'm' monitor mode."));
    return;
  }
  if (noise >= BUF) {
    Serial.println(F("!! Too noisy while still. Turn the potentiometer until the DO LED"));
    Serial.println(F("   is off when still, then calibrate again."));
    return;
  }
  if (shake <= noise + 1) {
    Serial.println(F("!! Shake barely differs from still. Shake harder, adjust the pot,"));
    Serial.println(F("   or try a longer window (e.g. 'w 400') and recalibrate."));
    return;
  }

  uint16_t th = noise + max(1, (shake - noise) / 2);
  if (th > BUF) th = BUF;
  s.cfg.threshold = th;
  clearHistory(s);
  s.lastLevel = digitalRead(s.pin);

  Serial.print(F("Sensor ")); Serial.print(i + 1);
  Serial.print(F(" threshold = ")); Serial.println(s.cfg.threshold);
  Serial.println(F("Try it now. Type 'save' to keep it after reboot."));

  // resync the other sensor so time spent calibrating doesn't count
  for (uint8_t k = 0; k < NUM_SENSORS; k++) {
    clearHistory(sensors[k]);
    sensors[k].lastLevel = digitalRead(sensors[k].pin);
  }
}

// ================================================================
// Settings / EEPROM
uint16_t eepromAddr(uint8_t i) { return i * sizeof(Settings); }

void loadSettings(uint8_t i) {
  Settings &c = sensors[i].cfg;
  EEPROM.get(eepromAddr(i), c);
  bool ok = c.magic == MAGIC &&
            c.threshold >= 1 && c.threshold <= BUF &&
            c.windowMs >= 10 && c.windowMs <= 5000 &&
            c.cooldownMs <= 10000 &&
            c.debounceUs <= 20000;
  if (!ok) c = DEFAULTS;
}

void saveAll() {
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    sensors[i].cfg.magic = MAGIC;
    EEPROM.put(eepromAddr(i), sensors[i].cfg);
  }
  Serial.println(F("Saved both sensors to EEPROM."));
}

void showSettings(uint8_t i) {
  Sensor &s = sensors[i];
  Serial.print(F("--- sensor ")); Serial.print(i + 1);
  Serial.print(F(" (D")); Serial.print(s.pin); Serial.print(F(")"));
  if (i == sel) Serial.print(F("  <- selected"));
  Serial.println(F(" ---"));
  Serial.print(F("threshold  t = ")); Serial.print(s.cfg.threshold);  Serial.println(F(" edges"));
  Serial.print(F("window     w = ")); Serial.print(s.cfg.windowMs);   Serial.println(F(" ms"));
  Serial.print(F("cooldown   d = ")); Serial.print(s.cfg.cooldownMs); Serial.println(F(" ms"));
  Serial.print(F("debounce   b = ")); Serial.print(s.cfg.debounceUs); Serial.println(F(" us"));
  Serial.print(F("signal now   = ")); Serial.println(digitalRead(s.pin) ? F("HIGH") : F("LOW"));
  Serial.print(F("shakes seen  = ")); Serial.println(s.shakeCount);
}

void showAll() {
  for (uint8_t i = 0; i < NUM_SENSORS; i++) showSettings(i);
}

void showHelp() {
  Serial.println(F("--- commands (apply to the SELECTED sensor) ---"));
  Serial.println(F("1 / 2      select sensor 1 (D12) or 2 (D11)"));
  Serial.println(F("c          calibrate selected sensor"));
  Serial.println(F("t <n>      threshold: edges needed (1-32, lower = more sensitive)"));
  Serial.println(F("w <ms>     window to count edges in (10-5000)"));
  Serial.println(F("d <ms>     cooldown between shakes (0-10000)"));
  Serial.println(F("b <us>     debounce, ignore faster edges (0-20000)"));
  Serial.println(F("reset      restore defaults for selected sensor"));
  Serial.println(F("Shortcut: prefix a sensor number, e.g. '2 c' or '1 t 6'"));
  Serial.println(F("--- global ---"));
  Serial.println(F("m          toggle live monitor (both sensors, edges per 100 ms)"));
  Serial.println(F("s          show settings for both sensors"));
  Serial.println(F("save       store both sensors' settings in EEPROM"));
  Serial.println(F("load       reload both from EEPROM"));
}

bool setValue(uint8_t i, const char *name, uint16_t &field, long v, long lo, long hi) {
  if (v < lo || v > hi) {
    Serial.print(F("Out of range for ")); Serial.print(name);
    Serial.print(F(": ")); Serial.print(lo); Serial.print(F("-")); Serial.println(hi);
    return false;
  }
  field = (uint16_t)v;
  Serial.print(F("S")); Serial.print(i + 1); Serial.print(' ');
  Serial.print(name); Serial.print(F(" = ")); Serial.println(field);
  clearHistory(sensors[i]);
  return true;
}

void handleCommand(char *s) {
  while (*s == ' ') s++;
  if (*s == 0) return;

  // sensor number: alone selects, as a prefix applies one command to that sensor
  uint8_t target = sel;
  if (*s >= '1' && *s < '1' + NUM_SENSORS) {
    uint8_t n = *s - '1';
    s++;
    while (*s == ' ') s++;
    if (*s == 0) {
      sel = n;
      Serial.print(F("Selected sensor ")); Serial.print(sel + 1);
      Serial.print(F(" (D")); Serial.print(sensors[sel].pin); Serial.println(F(")"));
      return;
    }
    target = n;
  }

  if (!strcmp(s, "save"))  { saveAll(); return; }
  if (!strcmp(s, "load"))  { for (uint8_t i = 0; i < NUM_SENSORS; i++) loadSettings(i); showAll(); return; }
  if (!strcmp(s, "reset")) {
    sensors[target].cfg = DEFAULTS;
    clearHistory(sensors[target]);
    showSettings(target);
    return;
  }

  Settings &c = sensors[target].cfg;
  char cmd = s[0];
  long v = strtol(s + 1, NULL, 10);
  bool hasArg = false;
  for (char *p = s + 1; *p; p++) if (*p >= '0' && *p <= '9') { hasArg = true; break; }

  switch (cmd) {
    case 'h': case '?': showHelp(); break;
    case 's': showAll(); break;
    case 'c': calibrate(target); break;
    case 'm':
      monitorOn = !monitorOn;
      for (uint8_t i = 0; i < NUM_SENSORS; i++) sensors[i].monitorEdges = 0;
      monitorLastMs = millis();
      Serial.println(monitorOn ? F("Monitor ON (type m to stop)") : F("Monitor OFF"));
      break;
    case 't': if (hasArg) setValue(target, "threshold", c.threshold,  v, 1, BUF);   else showSettings(target); break;
    case 'w': if (hasArg) setValue(target, "window",    c.windowMs,   v, 10, 5000); else showSettings(target); break;
    case 'd': if (hasArg) setValue(target, "cooldown",  c.cooldownMs, v, 0, 10000); else showSettings(target); break;
    case 'b': if (hasArg) setValue(target, "debounce",  c.debounceUs, v, 0, 20000); else showSettings(target); break;
    default:
      Serial.print(F("Unknown: ")); Serial.println(s);
      Serial.println(F("Type h for help."));
  }
}

void handleSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = 0;
        for (uint8_t i = 0; i < lineLen; i++) lineBuf[i] = tolower(lineBuf[i]);
        handleCommand(lineBuf);
        lineLen = 0;
      }
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = ch;
    }
  }
}

void printBar(uint16_t n) {
  if (n > 20) n = 20;
  for (uint16_t k = 0; k < 20; k++) Serial.print(k < n ? '#' : ' ');
}

void runMonitor(unsigned long now) {
  if (!monitorOn || now - monitorLastMs < 100) return;
  monitorLastMs = now;
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    Sensor &s = sensors[i];
    Serial.print('S'); Serial.print(i + 1); Serial.print(' ');
    Serial.print(digitalRead(s.pin) ? 'H' : 'L');
    Serial.print(' ');
    if (s.monitorEdges < 10) Serial.print(' ');
    Serial.print(s.monitorEdges);
    Serial.print(F(" |"));
    printBar(s.monitorEdges);
    Serial.print(F("|  "));
    s.monitorEdges = 0;
  }
  Serial.println();
}

// ================================================================
void setup() {
  for (uint8_t m = 0; m < 8; m++) {
    pinMode(MOTOR_IA[m], OUTPUT);
    pinMode(MOTOR_IB[m], OUTPUT);
    stopMotor(m);
  }
  pinMode(PIN_LED, OUTPUT);
  Serial.begin(115200);
  delay(100);

  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    Sensor &s = sensors[i];
    memset(&s, 0, sizeof(Sensor));
    s.pin = SIGNAL_PINS[i];
    pinMode(s.pin, INPUT_PULLUP);
    s.lastLevel = digitalRead(s.pin);
    loadSettings(i);
  }

  Serial.println(F("2x SW-18010P shake sensors ready."));
  showAll();
  Serial.println(F("Type h for commands. '1 c' / '2 c' to calibrate each sensor."));
}

void loop() {
  unsigned long now = millis();

  handleSerial();
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    if (pollEdge(sensors[i])) onEdge(i, millis());
  }
  runMonitor(now);
  updateWaves(millis());

  if (ledOffAt && (long)(now - ledOffAt) >= 0) {
    digitalWrite(PIN_LED, LOW);
    ledOffAt = 0;
  }
}
