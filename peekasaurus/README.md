# Peekasaurus

An Arduino-powered cardboard dinosaur, built in three parts. Each part has its own sketch and wiring guide.

| Part | Board | What it does | Guide |
|---|---|---|---|
| Head | Arduino Uno or Nano | Reads the colour of a ball dropped in and lights a NeoPixel strip to match. Trained once with `colour-bench.html`. | [head/WIRING.md](head/WIRING.md) |
| Body | Arduino Mega 2560 | Two shake sensors each set off a wave across four motors. | [body/WIRING.md](body/WIRING.md) |
| Tail | Arduino Uno + CNC Shield V3 | A stepper wiggles the tail when a hand comes near the ultrasonic sensor. | [tail/WIRING.md](tail/WIRING.md) |

This folder isn't part of the website. `head/colour-bench.html` runs from your own computer over `localhost`; see the head guide.
