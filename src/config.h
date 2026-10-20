// HonuHuliHuli configuration: pins, motion and network constants.
#pragma once

#include <cstdint>
#include "driver/gpio.h"

namespace cfg {

// D1 mini ESP32 (MH-ET LIVE MiniKit layout) pins
constexpr gpio_num_t BUTTON_PIN   = GPIO_NUM_17;  // D3, pushbutton to GND
constexpr gpio_num_t UV_PIN       = GPIO_NUM_16;  // D4, UV relay (active high)
constexpr gpio_num_t STEP_PIN     = GPIO_NUM_18;  // D5, driver STEP
constexpr gpio_num_t ENABLE_PIN   = GPIO_NUM_19;  // D6, driver ENABLE (active high)
constexpr gpio_num_t OLED_SDA_PIN = GPIO_NUM_21;  // D2
constexpr gpio_num_t OLED_SCL_PIN = GPIO_NUM_22;  // D1
constexpr uint8_t    OLED_ADDR    = 0x3C;

constexpr bool BUTTON_ACTIVE_LOW = true;  // internal pull-up enabled
constexpr bool ENABLE_AT_BOOT    = true;  // energize the motor at startup

// Motion
constexpr int   FULL_STEPS_PER_REV = 200;
constexpr int   MICROSTEPS         = 32;    // must match the driver setting
constexpr float ACCELERATION       = 0.3f;  // rev/s^2, all speed changes
constexpr float CONTINUOUS_RPM     = 20.0f;
constexpr float INDEX_RPM          = 40.0f; // peak for index moves (short
                                            // moves never reach it)
constexpr int   DIVISIONS          = 8;     // index move is 1/DIVISIONS turn
constexpr int   STEP_PULSE_US      = 5;     // STEP high time

constexpr int STEPS_PER_REV   = FULL_STEPS_PER_REV * MICROSTEPS;
constexpr int STEPS_PER_INDEX = STEPS_PER_REV / DIVISIONS;
static_assert(STEPS_PER_REV % DIVISIONS == 0, "steps/rev not divisible");

// Button
constexpr int LONG_PRESS_MS = 1000;  // hold this long to start a timed run,
                                     // and this much longer for each step
constexpr int HOLD_RUN_MINUTES[] = {5, 10, 20, 30, 40, 50, 60};
constexpr int DEBOUNCE_MS   = 30;

// Network
constexpr const char* HOSTNAME    = "honuhuli";  // http://honuhuli.local/
constexpr const char* AP_SSID     = "HonuHuli";  // fits one OLED line
constexpr const char* AP_PASSWORD = "honuhuli";  // WPA2, >= 8 characters
constexpr const char* AP_IP       = "192.168.2.1";
constexpr int         STA_TIMEOUT_MS = 10000;

constexpr int DEFAULT_RUN_MINUTES = 5;

}  // namespace cfg
