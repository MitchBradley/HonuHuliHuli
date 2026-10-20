// HonuHuliHuli - pushbutton turntable indexer for the ESP32 (ESP-IDF).
//
// A 200 step/rev stepper drives the turntable directly through a STEP/DIR
// driver.  STEP and an active-high ENABLE are driven; DIR is hardwired.
//
//   Short press       index 1/8 turn from wherever the table is.  Presses
//                     accumulate into one smooth move, even while moving.
//   Hold 1 second     start a 5 minute run (after an index move in progress
//                     finishes).  Each further second held sets the run to
//                     10, 20, 30, 40, 50, then 60 minutes from that moment.
//   Any press while   decelerate to a stop at once.
//   rotating
//   Held at power-up  skip WiFi and start the setup access point.
//
// The UV relay is on while rotating continuously at speed.  A web panel
// (station mode at http://honuhuli.local/, or the setup access point)
// offers the same controls plus "run for N minutes".

#include <string>
#include <vector>

#include "config.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "motion.h"
#include "net.h"
#include "settings.h"
#include "web.h"

static const char* TAG = "honu";

// ---------------------------------------------------------------------------
// Button

static int64_t now_ms() {
    return esp_timer_get_time() / 1000;
}

static bool button_down() {
    return gpio_get_level(cfg::BUTTON_PIN) == (cfg::BUTTON_ACTIVE_LOW ? 0 : 1);
}

static bool held_at_boot = false;

static void button_task(void*) {
    bool raw = button_down();
    bool stable = raw;
    int64_t changed = now_ms(), pressed_at = 0;
    // A press held from power-up never counts: no long press, and its
    // release is not a short press.
    bool long_sent = stable;
    bool consumed = stable;
    int hold_step = -1;  // index into HOLD_RUN_MINUTES while held, -1 none
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5));
        int64_t t = now_ms();
        bool r = button_down();
        if (r != raw) {
            raw = r;
            changed = t;
            continue;
        }
        if (raw != stable && t - changed >= cfg::DEBOUNCE_MS) {
            stable = raw;
            if (stable) {  // press
                pressed_at = t;
                long_sent = false;
                consumed = false;
                if (motion::status().mode == motion::Mode::Run) {
                    motion::stop();
                    consumed = true;
                }
            } else if (!consumed) {  // short press released
                motion::index(1);
            }
        } else if (stable && !long_sent && t - pressed_at >= cfg::LONG_PRESS_MS) {
            long_sent = true;
            if (!consumed) {
                hold_step = 0;
                motion::start(cfg::HOLD_RUN_MINUTES[0] * 60);
                consumed = true;
            } else {
                hold_step = -1;  // press was a stop; don't start a run
            }
        } else if (stable && long_sent && hold_step >= 0) {
            // Each further second held lengthens the run, from that moment
            constexpr int n = sizeof(cfg::HOLD_RUN_MINUTES) / sizeof(cfg::HOLD_RUN_MINUTES[0]);
            int want = (int)((t - pressed_at) / cfg::LONG_PRESS_MS) - 1;
            if (want > n - 1) want = n - 1;
            if (want > hold_step) {
                hold_step = want;
                motion::start(cfg::HOLD_RUN_MINUTES[hold_step] * 60);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Display: only this task touches the OLED

enum class View { Signon, Station, AccessPoint };
static View view = View::Signon;
static std::string view_ip;
static SemaphoreHandle_t view_lock;

static void set_view(View v, const std::string& ip) {
    xSemaphoreTake(view_lock, portMAX_DELAY);
    view = v;
    view_ip = ip;
    xSemaphoreGive(view_lock);
}

static std::string fmt_time(int32_t s) {
    char b[16];
    int32_t m = s / 60;
    s %= 60;
    if (m < 60) {
        snprintf(b, sizeof(b), "%ld:%02ld", (long)m, (long)s);
    } else {
        snprintf(b, sizeof(b), "%ld:%02ld:%02ld", (long)(m / 60), (long)(m % 60), (long)s);
    }
    return b;
}

static void ui_task(void*) {
    std::string label;  // state row; kept while decelerating
    while (true) {
        xSemaphoreTake(view_lock, portMAX_DELAY);
        View v = view;
        std::string ip = view_ip;
        xSemaphoreGive(view_lock);

        std::vector<std::string> rows;
        if (v == View::Signon) {
            rows = {"", display::centered("Honu"), "", display::centered("HuliHuli"), ""};
        } else if (v == View::Station) {
            // mDNS name and IP address, centered
            for (const std::string& r : display::wrap(std::string(cfg::HOSTNAME) + ".local")) {
                rows.push_back(display::centered(r));
            }
            for (const std::string& r : display::wrap(ip)) rows.push_back(display::centered(r));
        } else {
            // Setup access point: SSID and the setup page address
            rows = {cfg::AP_SSID, ""};
            for (const std::string& r : display::wrap(ip + "/wifi")) rows.push_back(r);
        }
        rows.resize(5);

        motion::Status s = motion::status();
        switch (s.mode) {
            case motion::Mode::Idle: label = ""; break;
            case motion::Mode::Index: label = "Index"; break;
            case motion::Mode::Run: label = s.remaining_s >= 0 ? fmt_time(s.remaining_s) : "Run"; break;
            case motion::Mode::Stopping: break;
        }
        rows.push_back(display::centered(label));
        display::show(rows);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------------------------------------------------------------------------

extern "C" void app_main() {
    settings::init();
    motion::init();  // pins first: UV off, motor enabled, before slow WiFi

    gpio_config_t in = {};
    in.pin_bit_mask = 1ULL << cfg::BUTTON_PIN;
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = cfg::BUTTON_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    in.pull_down_en = cfg::BUTTON_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    gpio_config(&in);
    vTaskDelay(pdMS_TO_TICKS(cfg::DEBOUNCE_MS));  // let the pull settle
    held_at_boot = button_down();

    ESP_LOGI(TAG, "HonuHuliHuli: %d steps/rev, %d steps per 1/%d turn", cfg::STEPS_PER_REV,
             cfg::STEPS_PER_INDEX, cfg::DIVISIONS);

    view_lock = xSemaphoreCreateMutex();
    display::init();
    xTaskCreate(ui_task, "ui", 4096, nullptr, 3, nullptr);
    xTaskCreate(button_task, "button", 3072, nullptr, 5, nullptr);

    net::init();
    std::string ip;
    if (held_at_boot) {
        ESP_LOGI(TAG, "button held; starting access point");
    } else {
        ip = net::connect_station();
    }
    if (!ip.empty()) {
        set_view(View::Station, ip);
    } else {
        set_view(View::AccessPoint, net::start_ap());
    }
    web::start();
}
