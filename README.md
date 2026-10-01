# HonuHuliHuli

MicroPython pushbutton controller for a turntable driven directly by a
200 step/rev stepper through a STEP/DIR driver with an active-high ENABLE
(DIR hardwired).
Runs on any ESP32 variant with an RMT peripheral.

## Operation

| Button action | Result |
|---|---|
| Short press | Index 1/8 turn. Presses accumulate, including while moving. |
| Hold 1 s | Continuous rotation starts as soon as 1 s is reached. |
| Any press while rotating | Decelerate and stop on the next reachable 1/8 position. |

The UV relay output (`UV_PIN`) turns on once continuous rotation reaches
speed and off immediately on the stop press, so it is off while decelerating.

The power-up position is index position 0; all stops land on multiples of 1/8 turn from it.

## Configuration

Edit the constants at the top of `main.py`: `STEP_PIN`, `ENABLE_PIN`, `ENABLE_AT_BOOT`, `UV_PIN`, `BUTTON_PIN`,
`BUTTON_ACTIVE_LOW`, `MICROSTEPS`, `ACCELERATION` (rev/s²),
`CONTINUOUS_RPM`, `INDEX_RPM`, `STEP_PULSE_US`, `LONG_PRESS_MS`.
Choose GPIOs that are not strapping/flash pins on your board.

## Install

```
mpremote cp main.py :main.py
mpremote reset
```

## REPL testing

`main.py` currently sets everything up but does not start the pushbutton
loop. From the REPL (e.g. `mpremote repl`):

| Command | Result |
|---|---|
| `index(n)` | Index n eighths of a turn (default 1) and wait for it to finish. |
| `run(seconds)` | Rotate continuously, then stop on the next 1/8 position. |
| `step(n, interval_ms=10)` | Send n microsteps, then print the electrical position and ideal phase currents. |
| `enable(bool)` | Energize (True) or release (False) the motor. |
| `loop()` | Run the pushbutton controller (Ctrl-C to exit). |

The button is still checked while `index()` and `run()` are running.
Ctrl-C during a command stops the table abruptly.

## How it works

Step pulses come from the RMT peripheral. Python computes ~40 ms chunks of
pulse timings for a trapezoidal profile (exact constant acceleration via
v² += 2a per step) and keeps one chunk ready while the current one plays,
polling the button in between. Python throughput limits the step rate to
roughly 10-20 k steps/s; normal turntable speeds are far below that.
