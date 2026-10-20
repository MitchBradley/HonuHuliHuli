// Web control panel (start, stop, index, run for N minutes, live status)
// and the WiFi setup page, on ESP-IDF's HTTP server.  Request handling runs
// in the server's own task; motion timing is in the RMT interrupt, so web
// traffic cannot disturb the steps.

#include "web.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "motion.h"
#include "net.h"
#include "settings.h"

namespace web {

static const char* TAG = "web";

static const char PAGE[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>%TITLE%</title>
<style>
body{font-family:system-ui,sans-serif;max-width:24rem;margin:1rem auto;padding:0 1rem}
#status{font-size:1.5rem;margin:1rem 0}
.uv{color:#a0f;font-weight:bold}
button{font-size:1.2rem;padding:.6rem 1rem;margin:.3rem 0;width:100%}
input{font-size:1.2rem;width:5rem}
.row{display:flex;gap:.5rem;align-items:center}
.row button{flex:1}
</style></head><body>
<h2>%TITLE%</h2>
<div id="status">...</div>
<button onclick="cmd('start')">Start</button>
<button onclick="cmd('stop')">Stop</button>
<button id="index" onclick="cmd('index')">Index 1/8</button>
<div class="row">
<input id="minutes" type="number" min="1" max="1440" value="%MINUTES%"> min
<button onclick="cmd('run',{minutes:+document.getElementById('minutes').value})">Run</button>
</div>
<p><a href="/wifi">WiFi setup</a></p>
<script>
function fmt(s){var m=Math.floor(s/60);s%=60;return m+":"+(s<10?"0":"")+s}
function show(st){
  var t={idle:"Idle",moving:"Moving",running:"Running"}[st.state];
  if(st.remaining!==null)t+=" — "+fmt(st.remaining)+" left";
  if(st.uv)t+=' <span class="uv">UV on</span>';
  if(!st.enabled)t+=" (motor disabled)";
  document.getElementById("status").innerHTML=t;
  document.getElementById("index").disabled=st.state=="running";
}
function cmd(c,body){
  fetch("/"+c,{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify(body||{})}).then(r=>r.json()).then(show);
}
function poll(){fetch("/status").then(r=>r.json()).then(show).catch(()=>{
  document.getElementById("status").textContent="No connection"})}
poll();setInterval(poll,2000);
</script></body></html>
)HTML";

static const char WIFI_PAGE[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HonuHuliHuli WiFi%HOST%</title>
<style>
body{font-family:system-ui,sans-serif;max-width:24rem;margin:1rem auto;padding:0 1rem}
label{display:block;margin-top:.6rem}
input{font-size:1.1rem;width:100%;box-sizing:border-box}
button{font-size:1.2rem;padding:.6rem 1rem;margin-top:1rem;width:100%}
.note{color:#666;font-size:.9rem}
</style></head><body>
<h2>WiFi setup%HOST%</h2>
<form method="POST" action="/wifi">
<label>Network<input name="SSID" list="nets" value="%SSID%" required></label>
<datalist id="nets">%NETS%</datalist>
<label>Password<input name="Password" type="password"></label>
<label>IP address (or DHCP)<input name="IP" value="%IP%" required></label>
<label>Netmask<input name="netmask" value="%netmask%"></label>
<label>Gateway<input name="gateway" value="%gateway%"></label>
<label>DNS<input name="DNS" value="%DNS%"></label>
<p class="note">Netmask, gateway and DNS are only used with a static IP.
Saving reboots the turntable, which stops it immediately if it is moving.</p>
<button>Save and reboot</button>
</form>
<p><a href="/">Control panel</a></p>
</body></html>
)HTML";

// ---------------------------------------------------------------------------
// Helpers

static void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) {
        s.replace(p, from.size(), to);
    }
}

static std::string esc(const std::string& v) {
    std::string o;
    for (char c : v) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

// " (honuhuli.local)" when connected as a station, for page titles
static std::string host_suffix() {
    return net::station_connected() ? std::string(" (") + cfg::HOSTNAME + ".local)" : "";
}

static std::string read_body(httpd_req_t* req, size_t max) {
    std::string body;
    size_t len = req->content_len < max ? req->content_len : max;
    body.resize(len);
    size_t got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body.data() + got, len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        got += r;
    }
    body.resize(got);
    return body;
}

static esp_err_t send_status(httpd_req_t* req) {
    motion::Status s = motion::status();
    const char* state = s.mode == motion::Mode::Idle  ? "idle"
                        : s.mode == motion::Mode::Run ? "running"
                                                       : "moving";
    char rem[16];
    if (s.remaining_s >= 0) {
        snprintf(rem, sizeof(rem), "%ld", (long)s.remaining_s);
    } else {
        strcpy(rem, "null");
    }
    char json[128];
    snprintf(json, sizeof(json), "{\"state\": \"%s\", \"remaining\": %s, \"uv\": %s, \"enabled\": %s}",
             state, rem, s.uv ? "true" : "false", s.enabled ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error(httpd_req_t* req, const char* msg) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    std::string json = std::string("{\"error\": \"") + msg + "\"}";
    return httpd_resp_sendstr(req, json.c_str());
}

// application/x-www-form-urlencoded value decoding
static std::string url_decode(const char* s) {
    std::string o;
    for (; *s; s++) {
        if (*s == '+') {
            o += ' ';
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            o += (char)strtol(hex, nullptr, 16);
            s += 2;
        } else {
            o += *s;
        }
    }
    return o;
}

static std::string form_value(const std::string& body, const char* key) {
    char val[128];
    if (httpd_query_key_value(body.c_str(), key, val, sizeof(val)) != ESP_OK) return "";
    return url_decode(val);
}

static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// ---------------------------------------------------------------------------
// Handlers

static esp_err_t get_root(httpd_req_t* req) {
    std::string page = PAGE;
    replace_all(page, "%MINUTES%", std::to_string(settings::run_minutes()));
    replace_all(page, "%TITLE%", esc("HonuHuliHuli" + host_suffix()));
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page.data(), page.size());
}

static esp_err_t get_status(httpd_req_t* req) {
    return send_status(req);
}

static esp_err_t post_start(httpd_req_t* req) {
    read_body(req, 64);
    motion::start();
    return send_status(req);
}

static esp_err_t post_stop(httpd_req_t* req) {
    read_body(req, 64);
    motion::stop();
    return send_status(req);
}

static esp_err_t post_index(httpd_req_t* req) {
    read_body(req, 64);
    motion::index(1);
    return send_status(req);
}

static esp_err_t post_run(httpd_req_t* req) {
    std::string body = read_body(req, 256);
    size_t p = body.find("\"minutes\"");
    if (p == std::string::npos || (p = body.find(':', p)) == std::string::npos) {
        return send_error(req, "minutes required");
    }
    char* end;
    long minutes = strtol(body.c_str() + p + 1, &end, 10);
    if (end == body.c_str() + p + 1) return send_error(req, "minutes required");
    if (minutes < 1 || minutes > 1440) return send_error(req, "minutes must be 1-1440");
    settings::set_run_minutes((int)minutes);
    motion::start((uint32_t)minutes * 60);
    return send_status(req);
}

static esp_err_t get_wifi(httpd_req_t* req) {
    settings::Wifi w;
    settings::load_wifi(w);
    std::string nets;
    for (const std::string& n : net::networks()) nets += "<option value=\"" + esc(n) + "\">";
    std::string page = WIFI_PAGE;
    replace_all(page, "%NETS%", nets);
    replace_all(page, "%HOST%", esc(host_suffix()));
    replace_all(page, "%SSID%", esc(w.ssid));
    replace_all(page, "%IP%", esc(w.ip.empty() ? "DHCP" : w.ip));
    replace_all(page, "%netmask%", esc(w.netmask));
    replace_all(page, "%gateway%", esc(w.gateway));
    replace_all(page, "%DNS%", esc(w.dns));
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page.data(), page.size());
}

static void reboot(void*) {
    esp_restart();
}

static esp_err_t post_wifi(httpd_req_t* req) {
    std::string body = read_body(req, 1024);
    settings::Wifi w;
    w.ssid = trim(form_value(body, "SSID"));
    w.password = form_value(body, "Password");
    w.ip = trim(form_value(body, "IP"));
    w.netmask = trim(form_value(body, "netmask"));
    w.gateway = trim(form_value(body, "gateway"));
    w.dns = trim(form_value(body, "DNS"));
    httpd_resp_set_type(req, "text/html");
    if (w.ssid.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Network name required");
    }
    if (w.ip.empty() || strcasecmp(w.ip.c_str(), "DHCP") == 0) {
        w.ip = "DHCP";
    } else if (w.netmask.empty() || w.gateway.empty() || w.dns.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Static IP needs netmask, gateway and DNS");
    }
    settings::save_wifi(w);
    motion::stop();
    std::string msg = "Saved.  Rebooting to connect to " + esc(w.ssid) + "...";
    httpd_resp_sendstr(req, msg.c_str());

    // Reboot after the response has gone out
    static esp_timer_handle_t t = nullptr;
    if (!t) {
        esp_timer_create_args_t a = {};
        a.callback = reboot;
        a.name = "reboot";
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 1000000);
    return ESP_OK;
}

void start() {
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.max_uri_handlers = 12;
    c.stack_size = 8192;
    c.lru_purge_enable = true;
    httpd_handle_t server;
    if (httpd_start(&server, &c) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }
    struct Route {
        const char* uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t*);
    };
    static const Route routes[] = {
        {"/", HTTP_GET, get_root},          {"/status", HTTP_GET, get_status},
        {"/start", HTTP_POST, post_start},  {"/stop", HTTP_POST, post_stop},
        {"/index", HTTP_POST, post_index},  {"/run", HTTP_POST, post_run},
        {"/wifi", HTTP_GET, get_wifi},      {"/wifi", HTTP_POST, post_wifi},
    };
    for (const Route& r : routes) {
        httpd_uri_t u = {};
        u.uri = r.uri;
        u.method = r.method;
        u.handler = r.handler;
        httpd_register_uri_handler(server, &u);
    }
}

}  // namespace web
