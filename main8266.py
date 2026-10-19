# HonuHuliHuli for the ESP8266 (D1 mini) - pushbutton turntable indexer.
#
# Copy this file to the board as main.py, along with display.py and
# ssd1306.py.  Same behavior as the ESP32 version's button control:
#
#   Short press        index 1/8 turn.  Presses accumulate, so three quick
#                      presses (even while moving) index 3/8 turn.
#   Hold > 1 second    start continuous rotation (as soon as 1 s is reached).
#   Any press while    decelerate and stop exactly on the next 1/8 position
#   rotating           that can be reached at the configured acceleration.
#
# The ESP8266 has no RMT, so step pulses are timed in software: the main
# loop polls ticks_us() and toggles STEP when each step is due.  Timing
# jitters by tens of microseconds and pauses during garbage collection and
# OLED updates.  A late step shifts the schedule later rather than being
# caught up, so the motor never gets a burst of steps.  There is no WiFi
# or web interface in this variant.

from machine import Pin
from micropython import const
from time import ticks_ms, ticks_us, ticks_add, ticks_diff, sleep_ms
from math import sqrt

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

import pins              # D1 mini pin names (D3 = GPIO0: 10k pull-up;
                         # low at reset enters the bootloader)

BUTTON_PIN = pins.D3      # Pushbutton to GND
STEP_PIN = pins.D5        # Driver STEP input
ENABLE_PIN = pins.D6      # Driver ENABLE input (active high)
UV_PIN = pins.D7          # UV light relay (active high); on from reaching
                         # continuous speed until the stop press
ENABLE_AT_BOOT = True    # Energize the motor when the app starts
OLED_SDA_PIN = pins.D2
OLED_SCL_PIN = pins.D1
OLED_ADDR = const(0x3C)
STATE_ROW = const(5)     # OLED row for Index/Run
BUTTON_ACTIVE_LOW = True # True: button to GND, internal pull-up enabled

FULL_STEPS_PER_REV = const(200)
MICROSTEPS = const(32)   # Must match the driver's microstep setting

ACCELERATION = 0.3       # rev/s^2, used for all speed changes
CONTINUOUS_RPM = 20.0    # Speed for continuous rotation
INDEX_RPM = 40.0         # Top speed for 1/8 index moves

DIVISIONS = const(8)     # Index increment is 1/DIVISIONS of a turn
LONG_PRESS_MS = const(1000)
DEBOUNCE_MS = const(30)

# ---------------------------------------------------------------------------

STEPS_PER_REV = FULL_STEPS_PER_REV * MICROSTEPS
STEPS_PER_INDEX = STEPS_PER_REV // DIVISIONS

ACCEL_STEPS = ACCELERATION * STEPS_PER_REV       # steps/s^2
TWO_A = 2.0 * ACCEL_STEPS                         # v^2 change per step
CONT_V2 = (CONTINUOUS_RPM / 60.0 * STEPS_PER_REV) ** 2
INDEX_V2 = (INDEX_RPM / 60.0 * STEPS_PER_REV) ** 2
START_V2 = TWO_A          # Speed after one step from rest


class Motion:
    """Trapezoidal velocity profile, one step at a time.

    pos and target are absolute microstep counts from power-up, which is
    treated as index position 0.  target is None during continuous rotation.
    v2 is the square of the speed (steps/s) for the next step.
    """

    def __init__(self):
        self.pos = 0
        self.target = 0
        self.vmax2 = INDEX_V2
        self.v2 = START_V2
        self._cache_v2 = -1.0
        self._cache_us = 0

    def active(self):
        return self.target is None or self.pos < self.target

    def continuous(self):
        return self.target is None

    def at_speed(self):
        return self.target is None and self.v2 == self.vmax2

    def add_index(self):
        if self.target is not None:
            self.target += STEPS_PER_INDEX

    def start_continuous(self):
        self.target = None
        self.vmax2 = CONT_V2

    def stop_continuous(self):
        if self.target is not None:
            return
        # Steps needed to decelerate, rounded up to the next index boundary.
        # vmax stays at the continuous speed for any coast to the boundary.
        stop_steps = int((self.v2 - START_V2) / TWO_A) + 1
        p = self.pos + stop_steps
        self.target = -(-p // STEPS_PER_INDEX) * STEPS_PER_INDEX

    def interval_us(self):
        """Time from this step to the next, at the current speed."""
        v2 = self.v2
        if v2 != self._cache_v2:
            self._cache_v2 = v2
            self._cache_us = int(1_000_000 / sqrt(v2))
        return self._cache_us

    def advance(self):
        """Account for one step just taken and set the next step's speed."""
        self.pos += 1
        target = self.target
        v2 = self.v2
        if target is not None and target - self.pos <= (v2 - START_V2) / TWO_A:
            v2 -= TWO_A
            if v2 < START_V2:
                v2 = START_V2
        elif v2 < self.vmax2:
            v2 += TWO_A
            if v2 > self.vmax2:
                v2 = self.vmax2
        elif v2 > self.vmax2:
            v2 -= TWO_A
            if v2 < self.vmax2:
                v2 = self.vmax2
        self.v2 = v2
        if not self.active():
            # Move complete: next move starts from rest at index speed
            self.v2 = START_V2
            self.vmax2 = INDEX_V2


PRESS = const(1)
RELEASE = const(2)
LONG = const(3)


class Button:
    def __init__(self, gpio):
        pull = Pin.PULL_UP if BUTTON_ACTIVE_LOW else None
        self.pin = Pin(gpio, Pin.IN, pull)
        sleep_ms(DEBOUNCE_MS)    # let the pull settle
        self.raw = self.read()
        self.stable = self.raw
        self.changed_t = ticks_ms()
        self.press_t = 0
        # A press held from power-up never counts as a long press
        self.long_sent = self.stable

    def read(self):
        return self.pin.value() == (0 if BUTTON_ACTIVE_LOW else 1)

    def poll(self):
        """Return PRESS, RELEASE, LONG, or None."""
        now = ticks_ms()
        raw = self.read()
        if raw != self.raw:
            self.raw = raw
            self.changed_t = now
            return None
        if raw != self.stable and ticks_diff(now, self.changed_t) >= DEBOUNCE_MS:
            self.stable = raw
            if raw:
                self.press_t = now
                self.long_sent = False
                return PRESS
            return RELEASE
        if (self.stable and not self.long_sent
                and ticks_diff(now, self.press_t) >= LONG_PRESS_MS):
            self.long_sent = True
            return LONG
        return None


def fmt_time(s):
    """Seconds as m:ss, or h:mm:ss from an hour up."""
    m, s = divmod(s, 60)
    if m < 60:
        return "%d:%02d" % (m, s)
    h, m = divmod(m, 60)
    return "%d:%02d:%02d" % (h, m, s)


class Turntable:
    def __init__(self, oled=None):
        self.oled = oled
        self.state_label = ""
        self.shown_secs = None   # remaining seconds last shown
        if STEPS_PER_REV % DIVISIONS:
            raise ValueError("STEPS_PER_REV not divisible by DIVISIONS")
        self.enable_pin = Pin(ENABLE_PIN, Pin.OUT, value=ENABLE_AT_BOOT)
        self.step_pin = Pin(STEP_PIN, Pin.OUT, value=0)
        self.uv_pin = Pin(UV_PIN, Pin.OUT, value=0)
        self.uv_on = False
        self.button = Button(BUTTON_PIN)
        self.motion = Motion()
        self.next_t = None       # ticks_us when the next step is due
        # True when the current press has already been used (to stop
        # continuous rotation or to start it), so its release is not a
        # short press.  A press held from power-up is ignored.
        self.consumed = self.button.stable
        self.run_deadline = None # ticks_ms at which a timed run stops

    def start(self, seconds=None):
        """Start continuous rotation, optionally stopping after seconds."""
        self.motion.start_continuous()
        self.shown_secs = None
        if seconds is None:
            self.run_deadline = None
            self.show_state("Run")
        else:
            self.run_deadline = ticks_add(ticks_ms(), int(seconds * 1000))
            self.show_remaining()

    def stop(self):
        """Stop continuous rotation on the next reachable index position."""
        self.run_deadline = None
        if self.motion.continuous():
            self.uv(False)
            self.motion.stop_continuous()

    def show_remaining(self):
        """Count down a timed run on the state row, stopping at zero."""
        ms = ticks_diff(self.run_deadline, ticks_ms())
        if ms <= 0:
            self.stop()
            return
        secs = (ms + 999) // 1000
        if secs != self.shown_secs:
            self.shown_secs = secs
            self.show_state(fmt_time(secs))

    def show_state(self, label):
        """Show Index/Run on the OLED's bottom row; "" clears it, which
        leaves the signon (or access point info) above it."""
        if label != self.state_label:
            self.state_label = label
            if self.oled is not None:
                self.oled.center(STATE_ROW, label)

    def uv(self, on):
        self.uv_on = on
        self.uv_pin.value(1 if on else 0)

    def add_index(self, n=1):
        """Add n eighths of a turn to the current move (ignored while
        rotating continuously)."""
        if self.motion.continuous() or n < 1:
            return
        for _ in range(n):
            self.motion.add_index()
        self.show_state("Index")

    def poll(self):
        """One pass of button handling and step timing.  Returns True while
        moving."""
        motion = self.motion
        ev = self.button.poll()
        if ev == PRESS:
            self.consumed = False
            if motion.continuous():
                self.stop()
                self.consumed = True
        elif ev == LONG:
            if not self.consumed:
                self.start()
                self.consumed = True
        elif ev == RELEASE:
            if not self.consumed:
                self.add_index()

        if self.run_deadline is not None:
            self.show_remaining()

        if not motion.active():
            self.next_t = None
            if self.state_label:
                self.show_state("")
            return False
        now = ticks_us()
        if self.next_t is None:
            self.next_t = now
        late = ticks_diff(now, self.next_t)
        if late >= 0:
            p = self.step_pin
            p.on()
            p.off()
            iv = motion.interval_us()
            motion.advance()
            # Never catch up: a step more than one interval late resets the
            # schedule from now instead of bunching the following steps.
            base = now if late > iv else self.next_t
            self.next_t = ticks_add(base, iv)
            if motion.at_speed() and not self.uv_on:
                self.uv(True)
        return True

    def loop(self):
        """Run the pushbutton controller forever."""
        try:
            while True:
                if not self.poll():
                    sleep_ms(5)
        except KeyboardInterrupt:
            self.abort()
            raise

    def abort(self):
        m = self.motion
        m.target = m.pos
        m.v2 = START_V2
        m.vmax2 = INDEX_V2
        self.next_t = None
        self.run_deadline = None
        self.uv(False)
        self.show_state("")
        print("Stopped at step", m.pos)

    def wait(self):
        """Poll until all motion has finished."""
        try:
            while self.poll():
                pass
        except KeyboardInterrupt:
            self.abort()
            raise

    def index(self, n=1):
        """Index n eighths of a turn and wait for the move to finish."""
        if n < 1:
            raise ValueError("n must be >= 1")
        self.stop()
        self.add_index(n)
        self.wait()

    def run(self, seconds):
        """Rotate continuously for seconds, then stop on an index."""
        self.start(seconds)
        self.wait()

    def enable(self, on):
        self.enable_pin.value(1 if on else 0)


turntable = None
oled = None


def index(n=1):
    """REPL: index n eighths of a turn."""
    turntable.index(n)


def run(seconds):
    """REPL: rotate continuously for seconds, then stop on an index."""
    turntable.run(seconds)


def enable(on=True):
    """REPL: energize (True) or release (False) the motor."""
    turntable.enable(on)


def loop():
    """REPL: run the pushbutton controller (Ctrl-C to exit)."""
    turntable.loop()


def main():
    global turntable, oled
    import display
    oled = display.Display(OLED_SDA_PIN, OLED_SCL_PIN, OLED_ADDR)
    oled.center(1, "Honu")
    oled.center(3, "HuliHuli")
    turntable = Turntable(oled)
    print("HonuHuliHuli (ESP8266): %d steps/rev, %d steps per 1/%d turn"
          % (STEPS_PER_REV, STEPS_PER_INDEX, DIVISIONS))
    print("REPL commands: index(n), run(seconds), enable(bool), loop()")
    try:
        loop()
    except KeyboardInterrupt:
        print("Interrupted; REPL commands available")


if __name__ == "__main__":
    main()
