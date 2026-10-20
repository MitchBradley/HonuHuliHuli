# D1 mini form-factor pin names (A0, D0-D8, TX, RX) as GPIO numbers, for the
# board this is running on:
#
#   ESP8266 D1 mini       Wemos/LOLIN D1 mini (the original)
#   ESP32-S2 Mini         LOLIN S2 Mini, outer (D1 mini compatible) rows
#   ESP32 D1 mini         MH-ET LIVE MiniKit layout and its clones
#
# The board is picked from sys.platform and os.uname().machine; set BOARD
# to one of the table names to override that.
#
#   import pins
#   step = Pin(pins.D5, Pin.OUT)

import sys
import os

BOARD = None

_TABLES = {
    #               A0  D0  D1  D2  D3  D4  D5  D6  D7  D8  TX  RX
    "ESP8266":    ( 0, 16,  5,  4,  0,  2, 14, 12, 13, 15,  1,  3),
    "ESP32S2":    ( 3,  5, 35, 33, 18, 16,  7,  9, 11, 12, 39, 37),
    "ESP32":      (36, 26, 22, 21, 17, 16, 18, 19, 23,  5,  1,  3),
}
# Notes, by D1 mini position:
#   D1/D2   I2C SCL/SDA (OLED shield)
#   D3/D4   ESP8266: GPIO0/GPIO2 strapping pins with 10k pull-ups;
#           GPIO0 low at reset enters the bootloader, GPIO2 drives the LED
#   D5-D8   SPI SCK/MISO/MOSI/CS.  ESP8266 D8 (GPIO15) has a pull-down and
#           must be low at reset
#   A0      ESP8266: the ADC (machine.ADC(0)), not a GPIO number
#   TX/RX   ESP8266 and ESP32: UART0, the REPL console.  ESP32-S2 Mini:
#           plain GPIOs in the TX/RX positions (its REPL is native USB)


def _detect():
    if sys.platform == "esp8266":
        return "ESP8266"
    if sys.platform == "esp32":
        m = os.uname().machine.upper().replace("-", "")
        if "ESP32S2" in m:
            return "ESP32S2"
        for other in ("ESP32S3", "ESP32C3", "ESP32C6", "ESP32H2", "ESP32P4"):
            if other in m:
                raise ValueError("pins.py has no table for " + other)
        return "ESP32"
    raise ValueError("pins.py has no table for " + sys.platform)


BOARD = BOARD or _detect()
A0, D0, D1, D2, D3, D4, D5, D6, D7, D8, TX, RX = _TABLES[BOARD]
