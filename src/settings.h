// Persistent settings in NVS.
#pragma once

#include <string>

namespace settings {

struct Wifi {
    std::string ssid;
    std::string password;
    std::string ip = "DHCP";  // "DHCP" or a static address
    std::string netmask;
    std::string gateway;
    std::string dns;
};

void init();

bool load_wifi(Wifi& w);  // false if nothing saved
void save_wifi(const Wifi& w);

int  run_minutes();       // last "run for N minutes" value
void set_run_minutes(int n);

}  // namespace settings
