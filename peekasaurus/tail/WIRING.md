# Peekasaurus tail: wiring

The sketch is `tail.ino`. It runs on an **Arduino Uno** with a **CNC Shield V3**, one **A4988 (or DRV8825) stepper driver**, one **NEMA 17 stepper motor** and an **HC-SR04 ultrasonic sensor**.

The motor is mounted with its **shaft pointing straight up**, so the tail swings side to side in a horizontal plane.

- When a hand comes within **3–25 cm** of the sensor, the tail does **2 smooth sine-wave wiggles** (0.7 per second, about 2.9 s total), then returns to centre.
- The sensor is ignored while wiggling. The tail won't wiggle again until the hand moves **out of range** and comes back.

## Parts

| Qty | Part |
|---|---|
| 1 | Arduino Uno (R3 or compatible) |
| 1 | CNC Shield V3 |
| 1 | A4988 or DRV8825 stepper driver module |
| 1 | NEMA 17 stepper motor (4-wire, bipolar) |
| 1 | HC-SR04 ultrasonic distance sensor |
| 1 | Motor power supply: **12 V, 2 A** (A4988 accepts 8–35 V, DRV8825 8.2–45 V) |
| – | 4 female-to-female jumper wires for the sensor |

## Stepper driver and motor (Y axis)

The sketch uses **STEP = D3, DIR = D6, EN = D8**. On the CNC Shield V3 that is the **Y axis** slot.

| Signal | Arduino pin | On the shield |
|---|---|---|
| STEP | D3 | Y axis driver (wired internally) |
| DIR | D6 | Y axis driver (wired internally) |
| ENABLE | D8 | Shared EN for all drivers (wired internally) |

1. Plug the driver into the **Y** socket. Match the driver's **EN** pin to the shield's **EN** corner. On the A4988 the trim pot usually ends up nearest the **bottom** of the shield (away from the power terminal); check your board's markings. A backwards driver burns out instantly.
2. Plug the motor's 4-pin connector into the **Y** motor header (next to the Y socket). The pins pair up as coil A (`1A 1B`) then coil B (`2A 2B`).
3. Leave the **X**, **Z** and **A** sockets empty.

### Microstepping jumpers

The three jumpers under the Y socket (M0 / M1 / M2) set the step size. `SWING_STEPS = 200` means very different swings depending on them:

| Jumpers fitted | Step size | Total swing for 200 steps |
|---|---|---|
| none | full step | 360° (±180°), too much for a tail |
| M0 | 1/2 | 180° (±90°) |
| M1 | 1/4 | 90° (±45°) |
| M0 + M1 | 1/8 | 45° (±22.5°) |
| M0 + M1 + M2 | 1/16 (A4988) | 22.5° (±11.25°) |

Pick one that looks right, or change `SWING_STEPS` in the sketch instead. Smaller step sizes also make the motion smoother and quieter. (The DRV8825 also offers 1/32 with all three jumpers.)

## HC-SR04 sensor

The shield covers D9 and D10, but they're broken out on the **endstop headers**:

| HC-SR04 pin | Arduino pin | On the shield |
|---|---|---|
| Trig | D9 | **X endstop** header, signal pin (`X-` or `X+`, they're joined) |
| Echo | D10 | **Y endstop** header, signal pin (`Y-` or `Y+`, they're joined) |
| VCC | 5V | Any **5V** pin on the shield's top-right header row |
| GND | GND | Any **GND** pin (each endstop pair also has one) |

Each endstop position has two pins. The one **closer to the board edge** is usually GND and the inner one is the signal; check the silkscreen on your shield. The endstop headers have no 5V, so take VCC from the 5V pin on the header row by the reset button / spindle pins.

Point the sensor where a hand would approach the dinosaur, and keep it away from the moving tail so the tail doesn't trigger itself.

## Power

```
12 V supply (+) ──► shield blue terminal "+" (labelled 12-36V)
12 V supply (–) ──► shield blue terminal "–"

Arduino powered by USB (or its own barrel jack) as usual
```

- **The blue terminal only powers the motor drivers.** It does not power the Arduino. Plug the Uno in by USB too.
- **Check polarity before plugging in.** Reversed power on the shield kills the drivers.
- **Never plug or unplug the motor while the 12 V supply is on.** That can destroy the driver.
- Ground is shared through the shield, so no extra ground wire is needed.

### Set the driver current

Before running for long, set the current limit with the trim pot on the driver so the motor and driver don't overheat:

- **A4988:** Vref = motor rated current × 8 × Rs. With the common 0.1 Ω sense resistors (`R100`), Vref = current × 0.8. For a 1.0 A motor, that's about **0.8 V**.
- **DRV8825:** Vref = motor rated current ÷ 2. For a 1.0 A motor, that's **0.5 V**.

Measure between the trim pot and GND with the 12 V supply on and the motor connected. Turn it lower if the motor runs hot; turn it up if the motor skips steps.

Note: `EN_PIN` is held LOW for the whole time, so the motor is **always energised** and holds the tail firmly. It will get warm even when idle. That's normal, as long as you can still touch it.

## Pin overview

```
Uno pin   Goes to
-------   ---------------------------------------------
D3        Y driver STEP (via shield)
D6        Y driver DIR  (via shield)
D8        Driver ENABLE (via shield, LOW = on)
D9        HC-SR04 Trig  (X endstop header)
D10       HC-SR04 Echo  (Y endstop header)
5V / GND  HC-SR04 VCC / GND
```

## Checking it works

1. Fit the jumpers and set the driver current (see above).
2. Before powering, turn the tail by hand to the **centre position**. The sketch treats wherever the tail is at power-up as centre.
3. Upload `tail.ino`, then open the Serial Monitor at **9600 baud**. You should see `Tail ready!`.
4. Plug in the 12 V supply.
5. Move your hand to about 15 cm from the sensor. You should see `Wiggling!`, the tail should swing twice, return to centre, then print `Wiggle finished!`.
6. Pull your hand away and bring it back to trigger it again.

## Tuning

All settings are at the top of the sketch:

| Setting | Default | What it does |
|---|---|---|
| `SWING_STEPS` | 200 | Total travel, end to end (see the microstepping table) |
| `WIGGLE_SPEED` | 0.7 | Full wiggles per second |
| `WIGGLE_COUNT` | 2 | How many wiggles per trigger |
| `DETECT_DISTANCE` | 25 | Furthest distance (cm) that triggers |
| `MIN_DISTANCE` | 3 | Readings closer than this are ignored as noise |
| `SENSOR_INTERVAL` | 60 | Milliseconds between sensor readings |
| `COOLDOWN` | 500 | Milliseconds after a wiggle before the sensor is read again |

## Troubleshooting

| Problem | Fix |
|---|---|
| Motor hums or buzzes but doesn't turn | Coils wired wrong. Swap the middle two wires of the motor plug |
| Motor doesn't move at all | Driver in the wrong socket (must be **Y**), 12 V not connected, or current set too low |
| Tail swings the wrong way first | Flip the motor plug 180° on the header, or swap one coil's two wires |
| Swing is far too big or tiny | Change the microstepping jumpers or `SWING_STEPS` |
| Motor skips steps or tail drifts off centre | Raise the driver current a little, lower `WIGGLE_SPEED`, or use a lighter tail |
| Motor or driver gets too hot to touch | Lower the driver current. A small heatsink on the driver helps |
| Never triggers | Check Trig is on the X endstop and Echo on the Y endstop (not swapped), and that VCC is 5V |
| Triggers on its own | Something is within 25 cm (the tail, a wall, the table). Re-aim the sensor or lower `DETECT_DISTANCE` |
