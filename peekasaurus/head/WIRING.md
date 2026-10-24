# Peekasaurus head: wiring and training

The sketch is `head.ino`. It runs on an **Arduino Uno or Nano** with a **TCS34725 colour sensor** and a **64-pixel NeoPixel strip**.

Drop a ball into the head, the sensor reads its colour, and the strip lights up to match. The colours are **taught, not hard-coded** — you show the sensor each ball once and it learns what that ball looks like under this particular lighting and geometry. That makes it work with any set of balls, but it does mean the head has to be trained before it can do anything.

`colour-bench.html` is a browser page for training it. It talks to the board over USB and gives you buttons instead of typed commands. You need it (or a Serial Monitor) to set the head up — but **not** to run it afterwards. See [Running without a computer](#running-without-a-computer).

> ⚠️ **The strip needs its own power supply.** 64 pixels can draw several amps. Running them from the Arduino will brown out the board. See [Power](#power).

## Parts

| Qty | Part |
|---|---|
| 1 | Arduino Uno or Nano |
| 1 | TCS34725 colour sensor breakout (the plain one — see note below) |
| 1 | NeoPixel strip, 64 pixels (WS2812B) |
| 1 | Break-beam sensor pair (IR emitter + receiver) — optional but recommended |
| 1 | Strip power supply: 5 V, **at least 2 A** |
| 1 | 300–500 Ω resistor (strip data line) |
| 1 | 1000 µF capacitor, 6.3 V or higher (across the strip's power) |
| – | Matte black card or paint to line the chamber |
| – | Jumper wires |

**Get the plain breakout, not the GY-33.** The GY-33 (square, with a black shroud) has its own microcontroller that pre-processes the colour and does its own white balance — it fights the calibration in this sketch. The plain rectangular or square CJMCU-type board wires straight to the sensor chip. Look for `VIN GND 3V3 SCL SDA INT LED` broken out on the header.

## Libraries

Both from the Arduino IDE's Library Manager:

- **Adafruit TCS34725**
- **Adafruit NeoPixel**

## Colour sensor

| Sensor pin | Arduino | Note |
|---|---|---|
| VIN | 5V | the board has its own regulator |
| GND | GND | |
| SDA | **A4** | fixed by the I²C hardware, can't be moved |
| SCL | **A5** | fixed |
| LED | **D3** | pulled high on the module: HIGH or floating = on, LOW = off |
| 3V3 | *leave unconnected* | ⚠️ this is an **output**. Don't feed 5 V into it |
| INT | *leave unconnected* | |

A4 and A5 aren't configurable — they're the only pins wired to the ATmega328P's I²C peripheral.

## Break beam

Optional, but it makes the head markedly more reliable. Set `PIN_BEAM = -1` in the sketch if you aren't fitting one.

| Beam wire | Arduino |
|---|---|
| Receiver OUT | **D4** |
| Receiver VCC | 5V |
| Receiver GND | GND |
| Emitter VCC / GND | 5V / GND |

### Why it helps

Without a beam, the head has no idea when a ball is present. It reads colour continuously and infers "a ball arrived" from the colour changing — then waits for `CONFIRM` readings in a row to agree, so a half-in-frame blur doesn't get reported as a real ball. That costs latency, and a ball falling past is only in view for 60–100 ms.

The beam removes the guesswork. The break *is* the signal that a ball is there, so:

- **No debounce needed.** One reading per ball instead of `CONFIRM` agreeing ones.
- **Nothing gets missed.** A ball that physically blocks the beam cannot go unnoticed, however fast it falls.
- **No false positives.** An empty chamber never triggers, so the `empty` class and the threshold stop being load-bearing for "is there a ball?".
- **The reading is taken at the right moment**, rather than whenever the loop happened to come round.

There's a subtlety the beam also fixes. The sensor integrates continuously and `getRawData` returns the integration that *already finished* — so a read taken the instant the beam breaks describes the chamber **before** the ball arrived. `BEAM_FLUSH` discards that stale one and takes the next. That's why a beam-triggered reading is trustworthy and not just fast.

### Where to mount it

Across the ball's path, **at or just above** the sensor window.

- **At the window** — the break coincides with the ball being in front of the sensor. Leave `BEAM_SETTLE_MS = 0`.
- **Above the window** — the break comes slightly early, which is useful on a fast drop. Raise `BEAM_SETTLE_MS` until the reading lands while the ball is in view. Start at 10–20 ms and tune.

### Checking it

Send `b` in the Serial Monitor and block the beam with your hand. It prints an edge each time the state changes.

If it reads `BROKEN` when nothing is in the way, flip `BEAM_ACTIVE_LOW` in the sketch.

## NeoPixel strip

| Strip wire | Goes to |
|---|---|
| DIN | **D2**, through the 300–500 Ω resistor |
| 5V | Strip power supply (+) |
| GND | Strip power supply (–) **and** Arduino GND |

**Keep the strip's light out of the sensor chamber.** If any of it reaches the photodiodes, a lit orange strip tints the next reading orange and the head ends up measuring its own display. Mount it outside the chamber or baffle it completely.

## Power

```
Strip supply (+) ──► NeoPixel 5V
Strip supply (–) ──► NeoPixel GND ──► Arduino GND   (common ground!)

Arduino 5V  ──► TCS34725 VIN
Arduino GND ──► TCS34725 GND
```

- **Don't power 64 pixels from the Arduino's 5V pin.** At full white they can pull nearly 4 A. The board will brown out and reset — which on a finished build means it silently stops working.
- **All grounds must be connected together.** Without a common ground the strip shows random colours or nothing at all.
- The 1000 µF capacitor goes across the strip's 5V and GND, close to the strip. It absorbs the current spike when all the pixels change at once.
- Power the Arduino over USB as usual.

## Pin overview

```
Arduino pin   Goes to
-----------   -------------------------------
D2            NeoPixel DIN (via resistor)
D3            TCS34725 LED
D4            Break-beam receiver OUT
A4 (SDA)      TCS34725 SDA
A5 (SCL)      TCS34725 SCL
5V / GND      TCS34725, break beam; GND also to strip supply (-)
```

## The chamber

The single biggest influence on whether this works is the chamber, not the code.

- **Fixed distance.** Every ball must sit the same distance from the sensor — around 10–20 mm. A cradle, a ledge, or a printed collimator tube. If balls sit at varying distances, readings vary and colours get confused.
- **Matte black inside.** A white or coloured interior bounces tinted light back onto the ball and washes out the saturation that separates one colour from another. Matte black card or paint. Not electrical tape — it's glossy.
- **Light-tight.** Cover the opening. Daylight and room light change through the day; the calibration doesn't.
- **Sensor LEDs only.** The two white LEDs on the breakout should be the only light source in the chamber.

Ping-pong balls are translucent and glossy, which is why training takes several samples per ball at different rotations.

---

## Running without a computer

This is the switch that decides whether the head needs a laptop attached. It's near the top of `head.ino`:

```cpp
const bool AUTO_LIVE = false;
```

| | `false` | `true` |
|---|---|---|
| **At power-up** | Waits for a command. Strip stays dark. | Starts classifying immediately. |
| **Needs a computer?** | Yes — `colour-bench.html` or a Serial Monitor has to start it | **No.** USB power alone is enough |
| **Use it for** | Calibrating and training | The finished build |

**Train with `false`, then set it to `true` and re-upload.**

Your training survives the re-upload. It lives in the Arduino's EEPROM, not in the sketch, so changing this line and uploading again doesn't lose calibration or taught colours.

The page still works with `AUTO_LIVE = true` — connecting sends a newline, which drops the sketch back to its command prompt so you can re-train or re-calibrate whenever you like.

> ⚠️ **One caveat with `true`.** *Any* serial byte drops the sketch out of live mode to its command prompt, where it waits for input that never comes — strip dark until it's power-cycled. Plugging in a USB cable, electrical noise, or a computer waking up can all do it. For a permanent install, power the Arduino from a plain USB charger rather than a computer, and nothing will ever send it a byte.

---

## Training the head

Do this once, with `AUTO_LIVE = false`.

### Set up the page

`colour-bench.html` uses Web Serial, which needs **Chrome, Edge, or Opera** on a desktop, and the page must be served over `localhost` — opening the file directly won't work. From the folder holding the file:

```
python3 -m http.server 8000
```

Then open `http://localhost:8000/colour-bench.html`.

**Close the Arduino IDE's Serial Monitor first.** Only one program can hold the port.

Click **Connect** and pick the board's port.

### 1. Check the gain

Click **White balance** with a white reference in the chamber, then open the **Serial log** at the bottom of the page and read what came back.

- Warns **saturated** → gain too high
- Warns **very dim** → gain too low
- Says neither → you're done, skip to step 2

To change it, you need the Serial Monitor (there's no gain control in the page). Disconnect the page, open the Serial Monitor at 115200 with line ending **Newline**, and send `g 1` if saturated or `g 16` if dim. Then reconnect the page.

**Changing gain invalidates the calibration**, so always redo white and black balance afterwards.

### 2. White balance

Put a **white reference** in the chamber, at the same height and distance a ball sits. A white ball is ideal; a disc printed in white PLA or a piece of matte white card works too. It doesn't have to be perfectly white — just neutral, matte, and *always the same one*.

Click **White balance**.

Avoid anything glossy, and don't use the chamber wall — it's at the wrong distance.

### 3. Black balance

Press **matte black card** flat against the sensor face. Not just an empty chamber — the walls still bounce plenty of light back.

Click **Black balance**.

### 4. Check the readings are stable

Drop a ball in and watch the **chroma** numbers in the readout on the right. Rotate the ball a quarter turn every second or so.

Those three numbers should hold steady to about ±0.01. If they jump around by 0.05 or more, you have a glare or distance problem — fix the chamber before training, or you'll be teaching it noise.

### 5. Teach each ball

For each ball: put it in, type a name, click **Teach**. Rotate it a quarter turn and teach again. **Four or five samples per ball.**

Watch `dist from centroid` in the log. The first sample always reads 0. After that, under about 0.02 is tight. If one sample comes back over 0.05 it caught a glare — take two or three extra to dilute it.

**Use plain colour names.** A class named `orange`, `yellow`, `green`, `blue` (or white, red, cyan, purple, violet, pink, brown, black) automatically lights the strip in a proper version of that colour. Anything else — `orange_ball`, `org` — falls back to the muddy value the sensor actually measured. You can always override a colour by clicking the ball or the swatch in the list.

**If you have no break beam, also teach the empty chamber** as `empty`. It costs one of the eight class slots and makes "no ball present" far more reliable than leaning on the threshold. With a beam fitted this is optional — the beam already knows when the chamber is empty — but it does no harm.

### 6. Check the separation

Click **Known colours** in the panel and look at the pairwise separation line, or send `l` in the Serial Monitor for the full table.

Every pair should sit comfortably above the threshold — ideally three times it. Anything flagged **too close** will get confused in use, and more training samples won't fix it. That's an optical problem: line the chamber matte black, tighten the geometry, or drop one of the two colours.

Greens and yellows are always the closest pair. That's expected — both reflect heavily in green and differ mainly in blue.

### 7. Set the threshold

The slider is **maximum allowed distance**, so *lower is stricter*. Dragging it to minimum makes the head reject everything.

A good value is about a third of your smallest pairwise separation. If the tightest pair is 0.15, use 0.05.

### 8. Go standalone

Set `AUTO_LIVE = true` in `head.ino`, upload, and unplug the laptop. The head now runs on USB power alone.

---

## Configuration reference

All near the top of `head.ino`.

### The two you'll actually change

| Constant | Default | What it does |
|---|---|---|
| `AUTO_LIVE` | `false` | `true` = runs standalone from power-up. See [above](#running-without-a-computer) |
| `NUM_PIXELS` | `64` | Must match your strip's real LED count |

### Strip

| Constant | Default | What it does |
|---|---|---|
| `PIN_STRIP` | `2` | NeoPixel data pin |
| `STRIP_BRIGHTNESS` | `40` | 0–255. **Only raise this once the strip has its own supply** |
| `HOLD_MS` | `2000` | How long the strip stays lit after the ball leaves |

### Detection

| Constant | Default | What it does |
|---|---|---|
| `CONFIRM` | `2` | **Only used when no beam is fitted.** Readings that must agree before a ball counts. Each is ~30 ms, so this is the detection latency. Missing balls → lower it; one ball reported twice → raise it |
| `MAX_CLASSES` | `8` | Maximum taught colours. EEPROM-backed, so changing it needs the `MAGIC` bump too |

### Break beam

| Constant | Default | What it does |
|---|---|---|
| `PIN_BEAM` | `4` | Receiver output pin. **`-1` disables the beam** and falls back to `CONFIRM` debouncing |
| `BEAM_ACTIVE_LOW` | `true` | `true` = receiver pulls LOW when broken. Flip it if `b` reads BROKEN with nothing in the way |
| `BEAM_SETTLE_MS` | `0` | Delay from break to read. Raise it if the beam sits **above** the sensor window |
| `BEAM_FLUSH` | `1` | Stale integrations discarded before reading. 1 is correct for a 24 ms integration; raise only if readings look like the previous ball |
| `BEAM_LOCKOUT_MS` | `250` | Ignores re-triggers after a read, so a bouncing ball isn't counted twice. **Lower it if you need to drop balls faster than ~4/second** |

### Sensor

| Constant | Default | What it does |
|---|---|---|
| `PIN_LED` | `3` | Sensor illumination LEDs. Set to `-1` if you wire them permanently on |
| `INTEG` | `..._24MS` | Integration time. **Don't change casually** — the sensor's maximum count scales with it, so calibration taken at one setting is meaningless at another |
| `SAMPLES_SLOW` | `4` | Readings averaged when accuracy matters (calibration, teaching) |
| `SAMPLES_FAST` | `1` | Readings averaged in live mode |
| `LED_WARMUP_MS` | `250` | Settling time after the LEDs come on |
| `SATURATED` | `9500` | White balance warns above this (ceiling is 10240) |
| `TOO_DIM` | `800` | White balance warns below this |

### Stored in EEPROM, not in the sketch

Set at runtime, survive re-uploads:

| Setting | Default | Command |
|---|---|---|
| Match threshold | `0.080` | `h 0.05` — lower is stricter |
| Sensor gain | `4x` | `g 1` / `g 4` / `g 16` / `g 60` — **redo white + black balance after** |
| White / black balance | — | `w` / `k` |
| Taught colours | — | `t <name>` |
| Strip colour override | auto | `p orange ff780a`, or `p orange auto` |

**`CONFIRM` and `HOLD_MS` live only in the sketch.** The page reads the board's verdict rather than classifying for itself, so there's only one copy of each — they can't drift apart.

## Serial commands

115200 baud, line ending **Newline**. Type `?` for this list.

| Command | What it does |
|---|---|
| `?` | Help |
| `w` | White balance |
| `k` | Black balance |
| `t <name>` | Teach: add a reading to class `<name>` |
| `b` | Beam monitor — check break-beam wiring and polarity |
| `c` | Classify once |
| `m` | Live classify — prints one line per ball |
| `s` | Live raw stream (this is what the page uses) |
| `l` | List calibration, classes, and the pairwise separation table |
| `d <name>` | Delete a class |
| `x` | Erase everything |
| `h <value>` | Set match threshold |
| `g <1\|4\|16\|60>` | Set sensor gain |
| `p <name> <rrggbb>` | Set the strip colour for a class |
| `p <name> auto` | Revert to the canonical/measured colour |

Any line sent while `m` or `s` is running stops it.

## Checking it works

1. Upload `head.ino` with `AUTO_LIVE = false`, open the Serial Monitor at **115200 baud**, line ending **Newline**.
2. You should see `Peekasaurus head - ball colour trainer` and the command list. If you see `! TCS34725 not found`, check SDA on A4, SCL on A5, and VIN on 5V.
3. Watch the sensor's white LEDs during a reading — they should flick on and off. If they're on constantly, the module has them hard-wired; set `PIN_LED = -1`.
4. Send `l`. Fresh board shows no classes and a note to run `w` and `k`.
5. Train it using the steps above.
6. Send `m` and drop balls in. Each should print one numbered line, and the strip should light in that colour and go dark about 2 seconds after the ball leaves.

## Troubleshooting

| Problem | Fix |
|---|---|
| `! TCS34725 not found` at startup | SDA must be A4, SCL must be A5. Check VIN is on 5V, not 3V3 |
| Nothing recognised, even at the loosest threshold | The slider is a *maximum distance* — low is strict. Drag it **right**, not left |
| Colours confused with each other | Check the separation table (`l`). Under ~0.15 for a pair is an optical problem: matte-black the chamber, fix the distance |
| Readings drift as the ball rotates | Glare or inconsistent distance. Shim the ball further out, or diffuse the LEDs with paper |
| Strip shows muddy colours | Class isn't named a plain colour word, or has an old `p` override. `l` shows which — `col auto` is clean, `col #xxxxxx` is overridden |
| Beam reads BROKEN with nothing in the way | Flip `BEAM_ACTIVE_LOW` in the sketch, then re-check with `b` |
| Beam never triggers | Check the receiver OUT is on D4 and the emitter is powered. Send `b` and block it by hand |
| Colour is right but one ball behind | Raise `BEAM_FLUSH` to 2 |
| Fast drops read the empty chamber | Beam is above the window and the ball hasn't arrived yet — raise `BEAM_SETTLE_MS` to 10-20 |
| Rapid drops get skipped | `BEAM_LOCKOUT_MS` is swallowing them. Lower it toward the real transit time |
| Ball detected then immediately lost | You're in Training mode, which tracks live. Switch to **Live** mode — that's the one that latches and holds |
| One ball reported two or three times | With a beam: raise `BEAM_LOCKOUT_MS`. Without: raise `CONFIRM` to 3 |
| Balls falling past undetected | Fit the break beam — this is exactly what it solves. Without one, lower `CONFIRM` to 1 or add a ledge so the ball pauses |
| Arduino resets when the strip lights | Strip is drawing from the Arduino. Give it its own 5 V supply with a common ground |
| Strip shows random colours | Missing common ground between the strip supply and the Arduino |
| Everything reads near-white / saturated | Gain too high. `g 1`, then redo `w` and `k` |
| Page won't connect | Chrome/Edge/Opera only, must be served over `localhost`, and the Serial Monitor must be closed |
| Page freezes on White balance | Stale copy. Re-download `colour-bench.html` |
| `multiple definition of ...` when compiling | Duplicate library files. Look for `* 2.cpp` / `* 3.cpp` in your Arduino libraries folder and delete them, then clear `~/Library/Caches/arduino/sketches` |
| Upload fails, `programmer is out of sync` | Live mode is streaming into the upload. Close the page and the Serial Monitor first. On a Nano clone also try Tools → Processor → *Old Bootloader* |
| Strip dark, head unresponsive after running a while | With `AUTO_LIVE = true`, a stray serial byte parks it at the command prompt. Power-cycle. Use a plain USB charger rather than a computer |
