// WiFi station / setup access point, and mDNS.
#pragma once

#include <string>
#include <vector>

namespace net {

void init();

// Connect with the settings saved from the setup page.  Returns the IP
// address, or "" if there are none or the connection failed.
std::string connect_station();

// Scan for networks (for the setup page), then start the setup access
// point.  Returns its IP address.
std::string start_ap();

bool station_connected();

// SSIDs seen by the scan in start_ap()
const std::vector<std::string>& networks();

}  // namespace net
