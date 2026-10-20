// Step generation for the turntable.
//
// Each move is one RMT transmission.  The RMT's simple encoder calls
// step_encoder() from the refill interrupt whenever half of the channel's
// 64-symbol memory has been sent, and step_encoder() decides the timing of
// each next step from the motion state: accelerate, hold, or decelerate.
// One symbol is one step (STEP high for STEP_PULSE_US, then low for the
// rest of the interval).  The pulse timing is therefore entirely in
// hardware, and the only software deadline is refilling 32 symbols before
// the other 32 have been sent (~15 ms at full speed), which an interrupt
// meets regardless of WiFi or web traffic.  A move ends when the encoder
// reports done; the RMT is never cut off mid-transmission.
//
// Accelerating from rest, step k (0-based) ends at t(k+1) = sqrt(2(k+1)/a),
// so the precomputed interval table ramp[k] = t(k+1) - t(k) gives exact
// constant acceleration.  "Level" k is the number of acceleration steps
// taken; holding a speed repeats ramp[k-1]; decelerating plays the table
// backwards.  A move of rem remaining steps accelerates while it can still
// decelerate in time (rem >= k + 2), holds while rem > k, and decelerates
// when rem == k, so it always ends exactly at rest.

#include "motion.h"

#include <cmath>
#include <cstring>

#include "config.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

namespace motion {

static const char* TAG = "motion";

constexpr uint32_t RESOLUTION_HZ = 1000000;  // 1 us RMT ticks
constexpr uint32_t MAX_INTERVAL  = 32767 + cfg::STEP_PULSE_US;  // 15-bit low

// Ramp table in internal RAM: the encoder runs in an interrupt that must
// work while flash (and so the cache) is disabled.
static uint16_t* ramp      = nullptr;
static int32_t   ramp_len  = 0;
static int32_t   k_cont    = 0;  // level at continuous speed
static int32_t   k_index   = 0;  // level at index peak speed
static uint32_t  cont_q8   = 0;  // cruise intervals in 1/256 us
static uint32_t  index_q8  = 0;

// Motion state, shared between tasks and the encoder interrupt
struct State {
    Mode     mode;
    int32_t  k;             // acceleration level
    int32_t  kmax;          // level not to exceed in this move
    int32_t  rem;           // steps remaining (Index/Stopping)
    uint32_t cruise_q8;     // hold interval at kmax
    uint32_t frac;          // fractional-tick accumulator for cruise
    int32_t  pending_index; // eighths queued behind a stop
    bool     run_after;     // continuous rotation queued behind an index move
    bool     uv;
};
static DRAM_ATTR State st = {};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool tx_active = false;  // an RMT transmission is in flight
static int64_t  run_deadline_us = 0;     // 0 = untimed
static bool     enabled = cfg::ENABLE_AT_BOOT;

static rmt_channel_handle_t chan = nullptr;
static rmt_encoder_handle_t enc  = nullptr;
static TaskHandle_t task = nullptr;

// ---------------------------------------------------------------------------
// Encoder (interrupt context; also called from rmt_transmit for the first
// chunk).  Everything it touches is in IRAM/DRAM.

static IRAM_ATTR void begin_run() {
    st.mode = Mode::Run;
    st.kmax = k_cont;
    st.rem = 1;  // not counted down while running
    st.cruise_q8 = cont_q8;
}

static IRAM_ATTR void begin_index(int32_t steps) {
    st.mode = Mode::Index;
    st.kmax = k_index;
    st.rem = steps;
    st.cruise_q8 = index_q8;
}

static IRAM_ATTR uint32_t cruise_interval() {
    st.frac += st.cruise_q8;
    uint32_t iv = st.frac >> 8;
    st.frac &= 0xff;
    return iv;
}

// Interval for the next step, or 0 when the move is over.
static IRAM_ATTR uint32_t next_interval() {
    if (st.rem == 0) {  // previous move finished at rest
        if (st.run_after) {
            st.run_after = false;
            begin_run();
        } else if (st.pending_index > 0) {
            begin_index(st.pending_index * cfg::STEPS_PER_INDEX);
            st.pending_index = 0;
        } else {
            st.mode = Mode::Idle;
            return 0;
        }
    }
    int32_t k = st.k;
    if (st.mode == Mode::Run) {
        if (k < k_cont) {
            st.k = k + 1;
            return ramp[k];
        }
        if (k > k_cont) {
            st.k = k - 1;
            return ramp[k - 1];
        }
        if (!st.uv) {  // light on once at speed
            st.uv = true;
            gpio_set_level(cfg::UV_PIN, 1);
        }
        return cruise_interval();
    }
    uint32_t iv;
    if (st.rem >= k + 2 && k < st.kmax) {  // accelerate
        iv = ramp[k];
        st.k = k + 1;
    } else if (st.rem > k) {               // hold
        iv = k == 0 ? ramp[0] : k >= st.kmax ? cruise_interval() : ramp[k - 1];
    } else {                               // decelerate (rem == k)
        st.k = k - 1;
        iv = ramp[k - 1];
    }
    st.rem--;
    return iv;
}

static IRAM_ATTR size_t step_encoder(const void*, size_t, size_t, size_t symbols_free,
                                     rmt_symbol_word_t* symbols, bool* done, void*) {
    size_t n = 0;
    portENTER_CRITICAL_SAFE(&mux);
    while (n < symbols_free) {
        uint32_t iv = next_interval();
        if (iv == 0) {
            *done = true;
            break;
        }
        symbols[n].level0    = 1;
        symbols[n].duration0 = cfg::STEP_PULSE_US;
        symbols[n].level1    = 0;
        symbols[n].duration1 = iv - cfg::STEP_PULSE_US;
        n++;
    }
    portEXIT_CRITICAL_SAFE(&mux);
    return n;
}

static IRAM_ATTR bool on_trans_done(rmt_channel_handle_t, const rmt_tx_done_event_data_t*, void*) {
    tx_active = false;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &woken);
    return woken == pdTRUE;
}

// ---------------------------------------------------------------------------
// Task side

static void uv_off_locked() {
    if (st.uv) {
        st.uv = false;
        gpio_set_level(cfg::UV_PIN, 0);
    }
}

static void build_tables() {
    const double a  = cfg::ACCELERATION * cfg::STEPS_PER_REV;       // steps/s^2
    const double vc = cfg::CONTINUOUS_RPM / 60.0 * cfg::STEPS_PER_REV;
    const double vi = cfg::INDEX_RPM / 60.0 * cfg::STEPS_PER_REV;
    const double iv_cont = 1e6 / vc, iv_index = 1e6 / vi;

    // Levels needed: until the ramp interval reaches the faster target
    int32_t n = (int32_t)(fmax(vc, vi) * fmax(vc, vi) / (2 * a)) + 2;
    ramp = (uint16_t*)heap_caps_malloc(n * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(ramp);
    k_cont = k_index = -1;
    int64_t prev = 0;
    for (int32_t k = 0; k < n; k++) {
        int64_t t = llround(1e6 * sqrt(2.0 * (k + 1) / a));
        int64_t iv = t - prev;
        prev = t;
        ramp[k] = (uint16_t)(iv > MAX_INTERVAL ? MAX_INTERVAL : iv);
        if (k_cont < 0 && iv <= iv_cont) k_cont = k;
        if (k_index < 0 && iv <= iv_index) k_index = k;
    }
    ramp_len = n;
    if (k_cont < 0) k_cont = n - 1;
    if (k_index < 0) k_index = n - 1;
    cont_q8  = (uint32_t)llround(iv_cont * 256);
    index_q8 = (uint32_t)llround(iv_index * 256);
    ESP_LOGI(TAG, "ramp %ld levels (%u bytes), continuous at level %ld, index peak level %ld",
             (long)n, (unsigned)(n * sizeof(uint16_t)), (long)k_cont, (long)k_index);
}

static void kick() {
    if (task) xTaskNotifyGive(task);
}

static void motion_task(void* arg) {
    // Created here so the RMT interrupt lands on this task's core (1),
    // away from the WiFi stack on core 0.
    rmt_tx_channel_config_t cc = {};
    cc.gpio_num = cfg::STEP_PIN;
    cc.clk_src = RMT_CLK_SRC_DEFAULT;
    cc.resolution_hz = RESOLUTION_HZ;
    cc.mem_block_symbols = 64;
    cc.trans_queue_depth = 4;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&cc, &chan));

    rmt_simple_encoder_config_t ec = {};
    ec.callback = step_encoder;
    ec.min_chunk_size = 1;
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&ec, &enc));

    rmt_tx_event_callbacks_t cbs = {};
    cbs.on_trans_done = on_trans_done;
    ESP_ERROR_CHECK(rmt_tx_register_event_callbacks(chan, &cbs, nullptr));
    ESP_ERROR_CHECK(rmt_enable(chan));
    xSemaphoreGive((SemaphoreHandle_t)arg);

    // Unused payload, but with the cache-safe ISR the driver requires it
    // to be in internal RAM (a const would land in flash)
    static DRAM_ATTR uint8_t dummy = 0;
    rmt_transmit_config_t tc = {};
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));

        if (run_deadline_us && esp_timer_get_time() >= run_deadline_us) {
            stop();
        }

        portENTER_CRITICAL(&mux);
        bool start_tx = st.mode != Mode::Idle && !tx_active;
        if (start_tx) tx_active = true;
        portEXIT_CRITICAL(&mux);
        if (start_tx) {
            // The encoder generates every step; the payload is unused
            ESP_ERROR_CHECK(rmt_transmit(chan, enc, &dummy, sizeof(dummy), &tc));
        }

        portENTER_CRITICAL(&mux);
        if (st.mode == Mode::Idle) uv_off_locked();
        portEXIT_CRITICAL(&mux);
    }
}

void init() {
    gpio_config_t out = {};
    out.pin_bit_mask = (1ULL << cfg::ENABLE_PIN) | (1ULL << cfg::UV_PIN);
    out.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out);
    gpio_set_level(cfg::UV_PIN, 0);
    gpio_set_level(cfg::ENABLE_PIN, cfg::ENABLE_AT_BOOT ? 1 : 0);

    build_tables();
    st.mode = Mode::Idle;

    SemaphoreHandle_t ready = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(motion_task, "motion", 4096, ready, 10, &task, 1);
    xSemaphoreTake(ready, portMAX_DELAY);
    vSemaphoreDelete(ready);
}

void start(uint32_t seconds) {
    portENTER_CRITICAL(&mux);
    switch (st.mode) {
        case Mode::Idle:
            st.k = 0;
            st.frac = 0;
            begin_run();
            break;
        case Mode::Index:
            st.run_after = true;
            break;
        case Mode::Stopping:
            begin_run();  // accelerate again from the current level
            break;
        case Mode::Run:
            break;
    }
    run_deadline_us = seconds ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    portEXIT_CRITICAL(&mux);
    kick();
}

void stop() {
    portENTER_CRITICAL(&mux);
    run_deadline_us = 0;
    st.run_after = false;
    if (st.mode == Mode::Run) {
        st.mode = Mode::Stopping;
        st.rem = st.k;   // decelerate from here to rest
        st.kmax = st.k;
        uv_off_locked();
    }
    portEXIT_CRITICAL(&mux);
}

void index(int n) {
    if (n < 1) return;
    portENTER_CRITICAL(&mux);
    switch (st.mode) {
        case Mode::Idle:
            st.k = 0;
            st.frac = 0;
            begin_index(n * cfg::STEPS_PER_INDEX);
            break;
        case Mode::Index:
            st.rem += n * cfg::STEPS_PER_INDEX;
            break;
        case Mode::Stopping:
            st.pending_index += n;
            break;
        case Mode::Run:
            break;  // ignored while running
    }
    portEXIT_CRITICAL(&mux);
    kick();
}

void enable(bool on) {
    enabled = on;
    gpio_set_level(cfg::ENABLE_PIN, on ? 1 : 0);
}

Status status() {
    Status s;
    portENTER_CRITICAL(&mux);
    s.mode = st.mode;
    s.uv = st.uv;
    int64_t dl = run_deadline_us;
    portEXIT_CRITICAL(&mux);
    s.enabled = enabled;
    if (s.mode == Mode::Run && dl) {
        int64_t left = dl - esp_timer_get_time();
        s.remaining_s = left > 0 ? (int32_t)((left + 999999) / 1000000) : 0;
    } else {
        s.remaining_s = -1;
    }
    return s;
}

}  // namespace motion
