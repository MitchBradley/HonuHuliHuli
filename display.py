# Character display on the Wemos/LOLIN OLED shield (SSD1306, 64x48, I2C).
#
# With the built-in 8x8 font that is 6 lines of 8 characters.  If the
# display is missing, every call is a no-op so the turntable still runs.

from machine import Pin, I2C
import ssd1306

WIDTH = 64
HEIGHT = 48
COLS = WIDTH // 8
ROWS = HEIGHT // 8


class Display:
    def __init__(self, sda, scl, addr=0x3C):
        self.lines = [""] * ROWS
        try:
            i2c = I2C(0, sda=Pin(sda), scl=Pin(scl), freq=400_000)
            self.oled = ssd1306.SSD1306_I2C(WIDTH, HEIGHT, i2c, addr)
        except OSError as e:
            print("No OLED:", e)
            self.oled = None
        self.clear()

    def _redraw(self):
        # A full update is ~400 bytes over I2C, a few ms; only redraw on
        # change so frequent status calls stay cheap.
        o = self.oled
        if o is None:
            return
        o.fill(0)
        for row, s in enumerate(self.lines):
            o.text(s, 0, row * 8)
        o.show()

    def clear(self):
        self.lines = [""] * ROWS
        self._redraw()

    def line(self, row, s):
        """Set one row (0-5) to s, truncated to 8 characters."""
        s = str(s)[:COLS]
        if 0 <= row < ROWS and self.lines[row] != s:
            self.lines[row] = s
            self._redraw()

    def show(self, *rows):
        """Replace the whole display with the given rows."""
        new = [str(s)[:COLS] for s in rows[:ROWS]]
        new += [""] * (ROWS - len(new))
        if new != self.lines:
            self.lines = new
            self._redraw()

    def center(self, row, s):
        """Set one row with s centered."""
        s = str(s)[:COLS]
        self.line(row, " " * ((COLS - len(s)) // 2) + s)
