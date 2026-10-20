#include "settings.h"

#include "config.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace settings {

static const char* NS = "honu";

void init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static std::string get_str(nvs_handle_t h, const char* key) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return "";
    std::string s(len, '\0');
    nvs_get_str(h, key, s.data(), &len);
    s.resize(len - 1);  // drop the terminator
    return s;
}

bool load_wifi(Wifi& w) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    w.ssid = get_str(h, "ssid");
    w.password = get_str(h, "pass");
    w.ip = get_str(h, "ip");
    w.netmask = get_str(h, "mask");
    w.gateway = get_str(h, "gw");
    w.dns = get_str(h, "dns");
    nvs_close(h);
    if (w.ip.empty()) w.ip = "DHCP";
    return !w.ssid.empty();
}

void save_wifi(const Wifi& w) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &h));
    nvs_set_str(h, "ssid", w.ssid.c_str());
    nvs_set_str(h, "pass", w.password.c_str());
    nvs_set_str(h, "ip", w.ip.c_str());
    nvs_set_str(h, "mask", w.netmask.c_str());
    nvs_set_str(h, "gw", w.gateway.c_str());
    nvs_set_str(h, "dns", w.dns.c_str());
    nvs_commit(h);
    nvs_close(h);
}

int run_minutes() {
    nvs_handle_t h;
    int32_t n = cfg::DEFAULT_RUN_MINUTES;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "runmin", &n);
        nvs_close(h);
    }
    return n;
}

void set_run_minutes(int n) {
    if (n == run_minutes()) return;  // avoid needless flash writes
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &h));
    nvs_set_i32(h, "runmin", n);
    nvs_commit(h);
    nvs_close(h);
}

}  // namespace settings
