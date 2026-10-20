# HonuHuliHuli

Pushbutton and web controller for a turntable driven directly by a 200
step/rev stepper through a STEP/DIR driver (active-high ENABLE, DIR
hardwired). ESP-IDF firmware for the classic ESP32 in the D1 mini form
factor, built with PlatformIO.

The earlier MicroPython version is in [micropython/](micropython/README.md).
It was retired because MicroPython could not keep the step pulses going
while serving web requests; see its README for the details.

## Operation

| Action | Result |
|---|---|
| Short press | Index 1/8 turn from wherever the table is. Presses accumulate into one smooth move, including while moving. |
| Hold 1 s | Continuous rotation starts as soon as 1 s is reached (after an index move in progress finishes). |
| Any press while rotating | Decelerate to a stop at once. |
| Held at power-up | Skip WiFi and start the setup access point. |

The UV relay output turns on once continuous rotation reaches speed and off
immediately when a stop is requested.

## Display

The Wemos/LOLIN 64x48 OLED shield shows the network name and address (the
mDNS name and IP in station mode; the AP name and setup URL in access point
mode) and, on the bottom row, `Index`, `Run`, or the time left in a timed run.

## Network

WiFi settings are entered on the setup page and kept in NVS. At boot the
board connects with them; if there are none, the connection fails, or the
button is held, it starts an access point, `HonuHuli` (password `honuhuli`),
with the setup page at http://192.168.2.1/wifi. Saving reboots and connects.

In station mode the board answers at http://honuhuli.local/. The control
panel has Start, Stop, Index 1/8 and Run for N minutes (N is remembered),
with live status.

## Configuration

Pins and motion settings are constants in [src/config.h](src/config.h):
pins in D1 mini positions (button D3, UV D4, STEP D5, ENABLE D6, OLED on
D2/D1), 32 microsteps, 0.3 rev/s², 20 rpm continuous, 40 rpm index peak.

## Build

```
pio run -e esp32 -t upload
pio device monitor
```

## How the steps are generated

Each move is a single RMT transmission. The RMT driver's simple encoder
calls a function from the refill interrupt whenever half of the channel's
64-symbol memory has been sent, and that function decides each next step
from the motion state: accelerate, hold or decelerate. The step timing is
therefore entirely in hardware; the only software deadline is refilling 32
symbols before the other 32 have been sent (about 15 ms at full speed),
which an interrupt meets regardless of WiFi and web traffic. The interrupt
is cache-safe, so it keeps running during flash writes.

Acceleration is exactly constant: step k from rest ends at
t(k+1) = sqrt(2(k+1)/a), and a precomputed table of those intervals is
played forward to speed up and backward to slow down. A move with `rem`
steps left accelerates while it can still stop in time, holds, and
decelerates when `rem` equals the current ramp level, so every move ends
exactly at rest. Stops and extensions just change `rem` while the
transmission keeps running, and a move ends when the encoder reports done;
the RMT is never cut off mid-transmission.
