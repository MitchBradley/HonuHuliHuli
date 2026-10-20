#include "net.h"

#include <algorithm>
#include <cstring>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "settings.h"

namespace net {

static const char* TAG = "net";

static esp_netif_t* sta_netif = nullptr;
static EventGroupHandle_t events;
constexpr EventBits_t CONNECTED = BIT0;
constexpr EventBits_t GOT_IP    = BIT1;

static bool station_mode = false;  // keep reconnecting when dropped
static std::vector<std::string> seen;

static void on_event(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(events, CONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events, CONNECTED | GOT_IP);
        if (station_mode) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(events, GOT_IP);
    }
}

void init() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    events = xEventGroupCreate();
    sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, cfg::HOSTNAME);
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // we keep our own
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);
}

static void start_mdns() {
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(cfg::HOSTNAME);
    mdns_instance_name_set("HonuHuliHuli");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
}

static std::string ip_string(esp_netif_t* nif) {
    esp_netif_ip_info_t info;
    esp_netif_get_ip_info(nif, &info);
    char s[16];
    esp_ip4addr_ntoa(&info.ip, s, sizeof(s));
    return s;
}

std::string connect_station() {
    settings::Wifi w;
    if (!settings::load_wifi(w)) {
        ESP_LOGI(TAG, "no saved WiFi settings");
        return "";
    }
    bool is_static = w.ip != "DHCP";
    if (is_static) {
        esp_netif_dhcpc_stop(sta_netif);
        esp_netif_ip_info_t info = {};
        esp_netif_str_to_ip4(w.ip.c_str(), &info.ip);
        esp_netif_str_to_ip4(w.netmask.c_str(), &info.netmask);
        esp_netif_str_to_ip4(w.gateway.c_str(), &info.gw);
        esp_netif_set_ip_info(sta_netif, &info);
        esp_netif_dns_info_t dns = {};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_str_to_ip4(w.dns.c_str(), &dns.ip.u_addr.ip4);
        esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    wifi_config_t wc = {};
    strncpy((char*)wc.sta.ssid, w.ssid.c_str(), sizeof(wc.sta.ssid));
    strncpy((char*)wc.sta.password, w.password.c_str(), sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    station_mode = true;
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    // With a static address there is no DHCP to wait for
    EventBits_t want = is_static ? CONNECTED : GOT_IP;
    EventBits_t got = xEventGroupWaitBits(events, want, pdFALSE, pdTRUE,
                                          pdMS_TO_TICKS(cfg::STA_TIMEOUT_MS));
    if (!(got & want)) {
        ESP_LOGW(TAG, "connection to %s failed", w.ssid.c_str());
        station_mode = false;
        esp_wifi_disconnect();
        esp_wifi_stop();
        return "";
    }
    std::string ip = ip_string(sta_netif);
    ESP_LOGI(TAG, "connected to %s as %s", w.ssid.c_str(), ip.c_str());
    start_mdns();
    return ip;
}

std::string start_ap() {
    station_mode = false;

    // Scan in station mode first, so the AP's channel isn't disturbed
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    if (esp_wifi_scan_start(nullptr, true) == ESP_OK) {
        // On the heap: 20 records are ~1.6 KB, too much for the main task
        uint16_t n = 20;
        std::vector<wifi_ap_record_t> recs(n);
        esp_wifi_scan_get_ap_records(&n, recs.data());
        for (int i = 0; i < n; i++) {
            std::string s((const char*)recs[i].ssid);
            if (!s.empty() && std::find(seen.begin(), seen.end(), s) == seen.end()) seen.push_back(s);
        }
        std::sort(seen.begin(), seen.end());
    }
    esp_wifi_stop();

    esp_netif_t* ap = esp_netif_create_default_wifi_ap();
    esp_netif_dhcps_stop(ap);  // replace the default 192.168.4.1
    esp_netif_ip_info_t info = {};
    esp_netif_str_to_ip4(cfg::AP_IP, &info.ip);
    esp_netif_str_to_ip4(cfg::AP_IP, &info.gw);
    esp_netif_str_to_ip4("255.255.255.0", &info.netmask);
    esp_netif_set_ip_info(ap, &info);
    esp_netif_dhcps_start(ap);

    wifi_config_t wc = {};
    strncpy((char*)wc.ap.ssid, cfg::AP_SSID, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(cfg::AP_SSID);
    strncpy((char*)wc.ap.password, cfg::AP_PASSWORD, sizeof(wc.ap.password));
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.max_connection = 4;
    wc.ap.channel = 1;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "access point %s up; setup page at http://%s/wifi", cfg::AP_SSID, cfg::AP_IP);
    start_mdns();
    return cfg::AP_IP;
}

bool station_connected() {
    return station_mode && (xEventGroupGetBits(events) & CONNECTED);
}

const std::vector<std::string>& networks() {
    return seen;
}

}  // namespace net
