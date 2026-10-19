# Web control panel: start, stop, index, and run for N minutes.
#
# Handlers are plain (non-async) functions; on MicroPython microdot runs
# them directly in the asyncio loop, so they can call the Turntable's
# non-blocking commands without any locking.

import settings
import asyncio
import machine
import network
from microdot import Microdot
import wifi

PORT = 80
DEFAULT_MINUTES = 5


def _get_minutes():
    return settings.get("runminutes", DEFAULT_MINUTES)


def _set_minutes(n):
    settings.set("runminutes", n)


PAGE = """<!doctype html>
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
"""


WIFI_PAGE = """<!doctype html>
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
Saving reboots the turntable, which stops it immediately if it is moving.
wifi_credentials.py, if present, is still tried first.</p>
<button>Save and reboot</button>
</form>
<p><a href="/">Control panel</a></p>
</body></html>
"""


def _esc(v):
    return (str(v).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


async def _reboot():
    await asyncio.sleep_ms(1000)
    machine.reset()


def _host():
    """" (name.local)" when connected as a station, for page titles."""
    if network.WLAN(network.STA_IF).isconnected():
        return " (%s.local)" % wifi.HOSTNAME
    return ""


def _title():
    return "HonuHuliHuli" + _host()


def make_app(turntable):
    app = Microdot()

    @app.get("/")
    def index(request):
        page = (PAGE.replace("%MINUTES%", str(_get_minutes()))
                .replace("%TITLE%", _esc(_title())))
        return page, 200, {"Content-Type": "text/html"}

    @app.get("/status")
    def status(request):
        return turntable.status()

    @app.post("/start")
    def start(request):
        turntable.start()
        return turntable.status()

    @app.post("/stop")
    def stop(request):
        turntable.stop()
        return turntable.status()

    @app.post("/index")
    def index_move(request):
        turntable.add_index()
        return turntable.status()

    @app.post("/run")
    def run(request):
        try:
            minutes = int(request.json["minutes"])
        except (TypeError, KeyError, ValueError):
            return {"error": "minutes required"}, 400
        if not 1 <= minutes <= 1440:
            return {"error": "minutes must be 1-1440"}, 400
        _set_minutes(minutes)
        turntable.start(minutes * 60)
        return turntable.status()

    @app.get("/wifi")
    def wifi_page(request):
        cur = wifi.saved_settings() or wifi.file_settings() or {}
        page = WIFI_PAGE.replace("%NETS%", "".join(
            '<option value="%s">' % _esc(n) for n in wifi.networks))
        page = page.replace("%HOST%", _esc(_host()))
        for k in ("SSID", "IP", "netmask", "gateway", "DNS"):
            v = cur.get(k, "")
            if k == "IP" and not v:
                v = "DHCP"
            page = page.replace("%" + k + "%", _esc(v))
        return page, 200, {"Content-Type": "text/html"}

    @app.post("/wifi")
    async def wifi_save(request):
        f = request.form
        cfg = {k: (f.get(k) or "").strip() for k in wifi.KEYS}
        cfg["Password"] = f.get("Password") or ""
        if not cfg["SSID"]:
            return "Network name required", 400
        if cfg["IP"].upper() == "DHCP":
            cfg["IP"] = "DHCP"
        elif not all(cfg[k] for k in ("netmask", "gateway", "DNS")):
            return "Static IP needs netmask, gateway and DNS", 400
        wifi.save_settings(cfg)
        turntable.stop()
        asyncio.create_task(_reboot())
        return ("Saved.  Rebooting to connect to %s..." % _esc(cfg["SSID"]),
                200, {"Content-Type": "text/html"})

    return app
