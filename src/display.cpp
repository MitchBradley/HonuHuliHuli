#include "display.h"

#include <cstring>

#include "config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "font8x8.h"
#include "numfont.h"

namespace display {

static const char* TAG = "display";

constexpr int WIDTH  = 64;
constexpr int HEIGHT = 48;
constexpr int PAGES  = HEIGHT / 8;
// The SSD1306 has 128 columns; a 64-wide panel uses the middle ones
constexpr int COL_OFFSET = (128 - WIDTH) / 2;

static i2c_master_dev_handle_t dev = nullptr;
static uint8_t buf[1 + WIDTH * PAGES];  // control byte + framebuffer
static uint8_t shown[WIDTH * PAGES];    // what the panel has now
static bool shown_valid = false;

static void cmd(uint8_t c) {
    uint8_t b[2] = {0x80, c};
    i2c_master_transmit(dev, b, 2, 50);
}

bool init() {
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = I2C_NUM_0;
    bc.sda_io_num = cfg::OLED_SDA_PIN;
    bc.scl_io_num = cfg::OLED_SCL_PIN;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK || i2c_master_probe(bus, cfg::OLED_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no OLED found");
        return false;
    }
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = cfg::OLED_ADDR;
    dc.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &dev));

    // Same initialization as MicroPython's ssd1306 driver for 64x48
    static const uint8_t seq[] = {
        0xAE,        // display off
        0x20, 0x00,  // horizontal addressing
        0x40,        // start line 0
        0xA1,        // segment remap
        0xA8, HEIGHT - 1,
        0xC8,        // COM scan reversed
        0xD3, 0x00,  // display offset
        0xDA, 0x12,  // COM pins
        0xD5, 0x80,  // clock divide
        0xD9, 0xF1,  // precharge (internal VCC)
        0xDB, 0x30,  // VCOM deselect
        0x81, 0xFF,  // contrast
        0xA4,        // follow RAM
        0xA6,        // not inverted
        0xAD, 0x30,  // internal IREF
        0x8D, 0x14,  // charge pump on
        0xAF,        // display on
    };
    for (uint8_t c : seq) cmd(c);
    show({});
    return true;
}

static uint8_t* fb() {
    return buf + 1;
}

static void text_row(int r, const std::string& s) {
    for (int i = 0; i < COLS && i < (int)s.size(); i++) {
        unsigned ch = (unsigned char)s[i];
        if (ch < 32 || ch > 127) ch = '?';
        memcpy(fb() + r * WIDTH + i * 8, &font_petme128_8x8[(ch - 32) * 8], 8);
    }
}

// Send the framebuffer if it differs from what the panel shows
static void flush() {
    if (shown_valid && memcmp(shown, fb(), sizeof(shown)) == 0) return;
    memcpy(shown, fb(), sizeof(shown));
    shown_valid = true;
    cmd(0x21); cmd(COL_OFFSET); cmd(COL_OFFSET + WIDTH - 1);
    cmd(0x22); cmd(0); cmd(PAGES - 1);
    buf[0] = 0x40;  // data
    i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

void show(const std::vector<std::string>& rows) {
    if (!dev) return;
    memset(fb(), 0, WIDTH * PAGES);
    for (int r = 0; r < ROWS && r < (int)rows.size(); r++) text_row(r, rows[r]);
    flush();
}

void show_big(int number, const std::string& bottom) {
    if (!dev) return;
    memset(fb(), 0, WIDTH * PAGES);
    char digits[8];
    int n = snprintf(digits, sizeof(digits), "%d", number < 0 ? 0 : number);
    if (n > WIDTH / NUMFONT_WIDTH) n = WIDTH / NUMFONT_WIDTH;
    int x0 = (WIDTH - n * NUMFONT_WIDTH) / 2;
    for (int i = 0; i < n; i++) {
        const uint8_t(*g)[NUMFONT_WIDTH] = numfont[digits[i] - '0'];
        for (int p = 0; p < NUMFONT_PAGES; p++) {
            memcpy(fb() + p * WIDTH + x0 + i * NUMFONT_WIDTH, g[p], NUMFONT_WIDTH);
        }
    }
    text_row(ROWS - 1, centered(bottom));
    flush();
}

std::string centered(const std::string& s) {
    std::string t = s.substr(0, COLS);
    return std::string((COLS - t.size()) / 2, ' ') + t;
}

std::vector<std::string> wrap(std::string text) {
    std::vector<std::string> rows;
    while ((int)text.size() > COLS) {
        int cut = COLS;
        for (int i = COLS; i >= 1; i--) {
            if (text[i - 1] == '.' || text[i] == '/') {
                cut = i;
                break;
            }
        }
        rows.push_back(text.substr(0, cut));
        text = text.substr(cut);
    }
    rows.push_back(text);
    return rows;
}

}  // namespace display
