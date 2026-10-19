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

Edit the constants at the top of `main.py`: `STEP_PIN`, `ENABLE_PIN`, `ENABLE_AT_BOOT`, `UV_PIN`, `OLED_SDA_PIN`, `OLED_SCL_PIN`, `BUTTON_PIN`,
`BUTTON_ACTIVE_LOW`, `MICROSTEPS`, `ACCELERATION` (rev/s²),
`CONTINUOUS_RPM`, `INDEX_RPM`, `STEP_PULSE_US`, `LONG_PRESS_MS`.
Choose GPIOs that are not strapping/flash pins on your board.

## Display

A Wemos/LOLIN OLED shield (SSD1306 64x48, I2C at 0x3C) gives 6 lines of 8
characters through `display.py`. On the S2 Mini the shield's I2C is GPIO 33
(SDA) and 35 (SCL), so keep other functions off those pins. If no display is
found, the turntable runs without it. `ssd1306.py` is vendored from
micropython-lib (MIT).

## Web control panel

At boot, `main.py` connects to WiFi using `wifi_credentials.py` (gitignored;
same format as kiln-annealer's: `SSID`, `Password`, and `IP` set to either
`'DHCP'` or a static address plus `netmask`, `gateway`, `DNS`). It then serves
a control panel on port 80 with Start, Stop, Index 1/8, and Run for N
minutes, plus live status (state, time remaining, UV). The last N is saved
in `settings.json`.

If `wifi_credentials.py` doesn't connect, settings saved from the setup page
(in `settings.json`) are tried next. If those fail too, the turntable starts its own
access point, `HonuHuli` (password `honuhuli`; see `wifi.py`), and
shows its name and address on the OLED. Holding the button at power-up
skips straight to the access point. Join it
and browse to http://192.168.4.1/wifi to pick a network and enter the
password and IP settings; saving reboots and connects. The control panel
also works over the access point, and the setup page is linked from it.

The server is microdot 2.7.0 (asyncio), vendored in `microdot/`. The motion
loop runs as an asyncio task beside it.

## Install

```
mpremote cp -r microdot :
mpremote cp main.py web.py wifi.py display.py ssd1306.py settings.py wifi_credentials.py :
mpremote reset
```

## REPL commands

Ctrl-C at the console stops the controller (abruptly, if moving) and leaves
these available:

| Command | Result |
|---|---|
| `index(n)` | Index n eighths of a turn (default 1) and wait for it to finish. |
| `run(seconds)` | Rotate continuously, then stop on the next 1/8 position. |
| `step(n, interval_ms=10)` | Send n microsteps, then print the electrical position and ideal phase currents. |
| `enable(bool)` | Energize (True) or release (False) the motor. |
| `loop()` | Run the pushbutton controller only. |
| `web()` | Run the pushbutton controller and web server. |

The button is still checked while `index()` and `run()` are running.

## ESP8266 variant

`main8266.py` is a button-and-OLED-only version for the ESP8266 (D1 mini),
which has no RMT. Step pulses are timed in software from the main loop, so
they jitter and pause briefly during garbage collection and OLED updates; a
late step delays the schedule rather than being caught up. No WiFi or web.

Pins: button D3 (GPIO0; holding it at reset enters the bootloader), STEP D5,
ENABLE D6, UV D7, OLED on D2/D1.

```
mpremote cp main8266.py :main.py
mpremote cp display.py ssd1306.py :
mpremote reset
```

## How it works

Step pulses come from the RMT peripheral. Python computes ~40 ms chunks of
pulse timings for a trapezoidal profile (exact constant acceleration via
v² += 2a per step) and keeps one chunk ready while the current one plays,
polling the button in between. Python throughput limits the step rate to
roughly 10-20 k steps/s; normal turntable speeds are far below that.
