# Peekasaurus body: wiring

The sketch is `body.ino`. It runs on an **Arduino Mega 2560** with **8 TT gear motors** driven by **4 L9110S dual motor driver modules**, plus **2 SW-18010P shake sensor modules**.

- Shaking **sensor 1** makes **motors 1 → 4** move one after another as a 2-second wave.
- Shaking **sensor 2** does the same for **motors 5 → 8**.

> ⚠️ **Pin change:** motor 3's IB wire goes to **D53**, not D24. D24 is not used.

## Parts

| Qty | Part |
|---|---|
| 1 | Arduino Mega 2560 |
| 2 | SW-18010P shake (vibration) sensor modules, with VCC / GND / DO pins |
| 4 | L9110S dual motor driver modules (each drives 2 motors) |
| 8 | TT gear motors (3–6 V) |
| 1 | Motor power supply: 5–6 V, **at least 2 A** (for example 4×AA, or a 5 V 2–3 A adapter) |
| – | Jumper wires, breadboard or power rail |

## Shake sensors

| Sensor pin | Sensor 1 | Sensor 2 |
|---|---|---|
| DO | **D12** | **D11** |
| VCC | 5V | 5V |
| GND | GND | GND |

You can leave the AO pin unconnected if your module has one. The Mega's on-board LED (D13) flashes on every shake, so it needs no wiring.

## Motors

Each L9110S module has two channels, A and B. Each channel has an **IA** and an **IB** input and its own motor screw terminal.

| Motor | Module | Channel | IA (PWM) | IB (direction) | Shake sensor |
|---|---|---|---|---|---|
| 1 | L9110S #1 | A | D2 | D22 | Sensor 1 |
| 2 | L9110S #1 | B | D3 | D23 | Sensor 1 |
| 3 | L9110S #2 | A | D4 | **D53** ⚠️ | Sensor 1 |
| 4 | L9110S #2 | B | D5 | D25 | Sensor 1 |
| 5 | L9110S #3 | A | D6 | D26 | Sensor 2 |
| 6 | L9110S #3 | B | D7 | D27 | Sensor 2 |
| 7 | L9110S #4 | A | D8 | D28 | Sensor 2 |
| 8 | L9110S #4 | B | D9 | D29 | Sensor 2 |

On the module headers, the pins are labelled `A-IA`, `A-IB`, `B-IA`, `B-IB` (some boards use `A-1A`, `A-1B`, `B-1A`, `B-1B`).

IA must be on a PWM pin (D2–D9) because it controls speed. IB only sets direction, so any digital pin works for it. That's why motor 3's IB can move to D53.

## Power

```
Motor supply (+) ──► VCC on all 4 L9110S modules
Motor supply (–) ──► GND on all 4 L9110S modules ──► Arduino GND   (common ground!)

Arduino 5V ──► VCC on both shake sensors
Arduino GND ──► GND on both shake sensors
```

- **Don't power the motors from the Mega's 5V pin.** Eight TT motors can draw several amps at startup, which will reset the Arduino or damage it.
- **All grounds must be connected together.** Without a common ground the motors will twitch randomly or not move at all.
- Power the Mega through USB or its barrel jack as usual.

## Pin overview

```
Mega pin   Goes to
--------   -------------------------------
D2  - D9   L9110S IA inputs, motors 1-8 (PWM)
D11        Shake sensor 2 DO
D12        Shake sensor 1 DO
D13        On-board LED (no wiring)
D22, D23   L9110S IB, motors 1-2
D24        (unused, was motor 3 IB)
D25 - D29  L9110S IB, motors 4-8
D53        L9110S IB, motor 3
5V / GND   Shake sensors; GND also to motor supply (-)
```

## Checking it works

1. Upload `body.ino`, then open the Serial Monitor at **115200 baud** with the line ending set to **Newline**.
2. Type `m` to see live edge counts from both sensors. Turn each sensor module's potentiometer until it reads 0 when still and jumps up when tapped. Type `m` again to stop.
3. Calibrate each sensor: type `1 c` and follow the prompts, then do the same with `2 c`. Type `save` to store the settings in EEPROM. They survive unplugging.
4. Shake each sensor. You should see `sensor 1 shook 1` and `Starting motor wave 1` in the Serial Monitor, and the motors should ripple 1 → 2 → 3 → 4 (or 5 → 8 for sensor 2).

Type `h` in the Serial Monitor for the full list of tuning commands.

## Troubleshooting

| Problem | Fix |
|---|---|
| A motor spins the wrong way | Swap its two wires at the L9110S screw terminal |
| Motor 3 doesn't move but the others do | Check that its IB wire is on **D53**, not D24 |
| Arduino resets when motors start | The motors are drawing from the Arduino. Use a separate motor supply with a common ground |
| Motors twitch or don't move at all | Missing common ground between the motor supply and the Arduino |
| Shakes are missed | In Serial, lower the threshold (`1 t 3`) or recalibrate (`1 c`) |
| One shake triggers twice, or motor vibration retriggers | Raise the cooldown (`1 d 1500`) |
