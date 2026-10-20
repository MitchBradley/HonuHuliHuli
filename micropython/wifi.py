# WiFi setup, following kiln-annealer's connect_wifi().
#
# Station settings are tried in this order:
#   1. wifi_credentials.py (gitignored), which defines SSID, Password, and
#      IP, where IP is either 'DHCP' or a static address, in which case
#      netmask, gateway and DNS must be defined too.
#   2. Settings saved in settings.json from the web setup page (same keys).
# If neither connects, start_ap() brings up an access point so the setup
# page (and the control panel) can be reached at AP_IP.

import network
import time
import settings

HOSTNAME = "honuhuli"        # mDNS name: http://honuhuli.local/
AP_SSID = "HonuHuli"         # Fits one 8-character OLED line
AP_PASSWORD = "honuhuli"     # WPA2; must be at least 8 characters
AP_IP = "192.168.2.1"        # AP address; clients get DHCP leases on its /24

KEYS = ("SSID", "Password", "IP", "netmask", "gateway", "DNS")

networks = []                # SSIDs seen by the scan in start_ap()


def file_settings():
    try:
        import wifi_credentials as creds
    except ImportError:
        return None
    return {k: getattr(creds, k, "") for k in KEYS}


def saved_settings():
    return settings.get("wifi")


def save_settings(s):
    settings.set("wifi", {k: s.get(k, "") for k in KEYS})


def _try(sta, s, timeout_s):
    # A soft reboot leaves the WiFi driver running, possibly still in a
    # connect attempt from before, and connect() then fails with
    # "Wifi Internal State Error".  Cancel any such attempt first.
    try:
        sta.disconnect()
    except OSError:
        pass
    step = "ifconfig"
    try:
        if s["IP"] != "DHCP":
            sta.ifconfig((s["IP"], s["netmask"], s["gateway"], s["DNS"]))
        else:
            try:
                # Undo a static address set by an earlier attempt
                sta.ifconfig("dhcp")
            except (ValueError, TypeError, OSError):
                pass
        step = "connect"
        sta.connect(s["SSID"], s["Password"])
    except Exception as e:
        print("WiFi", step, e)
        return None
    for _ in range(timeout_s):
        if sta.isconnected():
            ip = sta.ifconfig()[0]
            print("Connected to", s["SSID"], "as", ip)
            return ip
        time.sleep(1)
    print("WiFi connection to", s["SSID"], "failed")
    return None


def set_hostname():
    # Must happen before the interface is activated; after a soft reboot
    # the interface is still up, so a change may need a hard reset.
    try:
        network.hostname(HOSTNAME)
    except (AttributeError, ValueError, OSError) as e:
        print("hostname", e)


def connect(timeout_s=10):
    """Connect as a station.  Returns the IP address, or None."""
    set_hostname()
    sta = network.WLAN(network.STA_IF)
    sta.active(True)
    if sta.isconnected():
        return sta.ifconfig()[0]
    for source, s in (("wifi_credentials.py", file_settings()),
                      ("saved settings", saved_settings())):
        if s:
            print("Trying", source)
            ip = _try(sta, s, timeout_s)
            if ip:
                return ip
    try:
        sta.disconnect()
    except OSError:
        pass
    return None


def start_ap():
    """Scan for networks (for the setup page), then start the access point.
    Returns the AP's IP address."""
    global networks
    sta = network.WLAN(network.STA_IF)
    sta.active(True)
    try:
        seen = [r[0].decode() for r in sta.scan()]
        networks = sorted(set(n for n in seen if n))
    except Exception as e:
        print("WiFi scan", e)
    # Stop the station so it doesn't channel-hop under the AP
    sta.active(False)

    ap = network.WLAN(network.AP_IF)
    ap.active(True)
    ap.config(essid=AP_SSID, password=AP_PASSWORD,
              authmode=network.AUTH_WPA_WPA2_PSK)
    # Replace MicroPython's default 192.168.4.1; the DHCP server follows
    ap.ifconfig((AP_IP, "255.255.255.0", AP_IP, AP_IP))
    print("Access point %s up; setup page at http://%s/wifi"
          % (AP_SSID, AP_IP))
    return ap.ifconfig()[0]
