# HonuHuliHuli

MicroPython pushbutton controller for a turntable driven directly by a
200 step/rev stepper through a STEP/DIR driver with an active-high ENABLE
(DIR hardwired).
Runs on any ESP32 variant with an RMT peripheral.

## Operation

| Button action | Result |
|---|---|
| Short press | Index 1/8 turn from wherever the table is. Presses accumulate, including while moving, and play as back-to-back 1/8 moves. |
| Hold 1 s | Continuous rotation starts as soon as 1 s is reached (after the current 1/8 move, if one is playing; queued moves are dropped). |
| Any press while rotating | Decelerate to a stop, starting within `CRUISE_MS` (0.1 s) at speed, or `PIECE_MS` (0.5 s) while still accelerating. |

The UV relay output (`UV_PIN`) turns on once continuous rotation reaches
speed and off immediately on the stop press, so it is off while decelerating.

Indexing is relative: each 1/8 move starts from wherever the table stopped.

## Configuration

Edit the constants at the top of `main.py`: `STEP_PIN`, `ENABLE_PIN`, `ENABLE_AT_BOOT`, `UV_PIN`, `OLED_SDA_PIN`, `OLED_SCL_PIN`, `BUTTON_PIN`,
`BUTTON_ACTIVE_LOW`, `MICROSTEPS`, `ACCELERATION` (rev/s²),
`CONTINUOUS_RPM`, `INDEX_RPM`, `STEP_PULSE_US`, `LONG_PRESS_MS`.
Pins are given as D1 mini names from `pins.py`, which picks the ESP8266,
ESP32-S2 Mini or ESP32 table for the board it runs on.

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

In station mode the board answers to `http://honuhuli.local/` (mDNS; `HOSTNAME`
in `wifi.py`), and the OLED shows that name and the IP address. The control
panel and setup page titles and headings also include it, e.g.
"HonuHuliHuli (honuhuli.local)", so bookmarks and tabs say which board they
are; over the access point they omit it.

If `wifi_credentials.py` doesn't connect, settings saved from the setup page
(in `settings.json`) are tried next. If those fail too, the turntable starts its own
access point, `HonuHuli` (password `honuhuli`; see `wifi.py`), and
shows its name and address on the OLED. Holding the button at power-up
skips straight to the access point. Join it
and browse to http://192.168.2.1/wifi (`AP_IP` in `wifi.py`) to pick a network and enter the
password and IP settings; saving reboots and connects. The control panel
also works over the access point, and the setup page is linked from it.

The server is microdot 2.7.0 (asyncio), vendored in `microdot/`. The motion
loop runs as an asyncio task beside it.

## Install

```
mpremote cp -r microdot :
mpremote cp main.py pins.py web.py wifi.py display.py ssd1306.py settings.py wifi_credentials.py :
mpremote reset
```

### Precompiled install

`./build_esp32.sh` puts everything for the ESP32 board in `build/esp32/`
(gitignored), with each module precompiled to `.mpy` by `mpy-cross` and
`main.py` (plus `wifi_credentials.py`, if present) as source. Upload the
folder's contents to the board root, and delete any `.py` copies of those
modules already there: MicroPython imports `foo.py` in preference to
`foo.mpy`.

## REPL commands

Ctrl-C at the console stops the controller (abruptly, if moving) and leaves
these available:

| Command | Result |
|---|---|
| `index(n)` | Index n eighths of a turn (default 1) and wait for it to finish. |
| `run(seconds)` | Rotate continuously, then decelerate to a stop. |
| `step(n, interval_ms=10)` | Send n microsteps, then print the electrical position and ideal phase currents. |
| `enable(bool)` | Energize (True) or release (False) the motor. |
| `stats()` | Report how late piece handoffs were since the last call (motion stutters show up here). |
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
mpremote cp pins.py display.py ssd1306.py :
mpremote reset
```

## How it works

Step pulses come from the RMT peripheral. At startup `main.py` precomputes
every pulse train it will play (constant acceleration via v² += 2a per step):
the ramp from rest to continuous speed cut into pieces of at most `PIECE_MS`,
the reverse of each ramp piece for deceleration, one `CRUISE_MS` cruise piece, and one
complete 1/8 index move. Running then needs no float math and creates no
garbage; whenever the RMT finishes a piece, `poll()` hands it the next
ready-made list. Because the ramp pieces are reversible, a stop during
acceleration turns around at the next piece boundary, and a restart during
deceleration climbs back from the same point.

MicroPython's `write_pulses()` waits for the previous transmission to finish,
so the motion is exposed to Python delays (web requests, garbage collection)
only at piece handoffs. Handoffs are driven by a one-shot `machine.Timer`
armed to fire `SPIN_US` before each piece ends; its callback waits out the rest
and writes the next piece. Timer callbacks are soft interrupts that run between
bytecodes of whatever Python code is executing, so a web request being handled
in the asyncio loop can't delay a handoff; only long-running C code can. The
remaining gap is `write_pulses()` converting the list (about 0.5 us per step),
which is why the cruise piece is short. After every move the console prints
handoff timing (also available from `stats()`).

### Known limitation

With a browser connected, handoffs can still be late by up to ~85 ms: even a
soft interrupt is held off while MicroPython is inside long-running C code
(socket/lwIP calls on the request path), and a gap in the pulses at speed
brakes the motor hard. Without a browser no late handoffs were seen. Two ways
around it were ruled out on the classic ESP32: cutting off a looping RMT
transmission (`rmt.loop(False)`) crashed even with ESP-IDF 5.5.2, and
MicroPython's `write_pulses()` cannot queue a transmission behind the playing
one. The robust fix is to move motion timing out of MicroPython, which is why
a C++ version is being written. The pieces never get cut off mid-transmission: on the classic ESP32,
`rmt_disable()` during a long transmission can panic (ESP-IDF issue 17692).

The tables take about 33 KB of RAM at the default settings; boot prints the
free memory left after building them.
