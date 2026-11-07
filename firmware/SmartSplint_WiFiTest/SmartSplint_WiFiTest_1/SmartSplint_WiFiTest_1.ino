
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// ------------------------------------------------------------------ settings
const char *WIFI_SSID     = "project";
const char *WIFI_PASSWORD = "12345678";
const char *HOSTNAME      = "smartsplint";   // page address: http://smartsplint.local


const bool REDUCE_TX_POWER = false;

WebServer server(80);
unsigned long lastWifiCheck = 0;

// ------------------------------------------------------------------ web page (stored in flash)
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Splint</title>
<style>
  :root { --bg:#f4f6f8; --card:#ffffff; --text:#1d2733; --muted:#6b7785; --accent:#1f6feb; --ok:#1a7f37; --bad:#cf222e; }
  @media (prefers-color-scheme: dark) {
    :root { --bg:#0d1117; --card:#161b22; --text:#e6edf3; --muted:#8b949e; --accent:#58a6ff; --ok:#3fb950; --bad:#f85149; }
  }
  * { box-sizing: border-box; }
  body { margin:0; font-family: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; background:var(--bg); color:var(--text); }
  header { padding:20px 16px 8px; max-width:900px; margin:auto; }
  h1 { margin:0; font-size:1.5rem; }
  .sub { color:var(--muted); margin-top:4px; font-size:0.95rem; }
  .dot { display:inline-block; width:10px; height:10px; border-radius:50%; background:var(--bad); margin-right:6px; }
  .dot.on { background:var(--ok); }
  main { max-width:900px; margin:auto; padding:8px 16px 32px; }
  h2 { font-size:1rem; color:var(--muted); margin:24px 0 8px; font-weight:600; }
  .grid { display:grid; grid-template-columns: repeat(auto-fill, minmax(190px, 1fr)); gap:12px; }
  .card { background:var(--card); border-radius:12px; padding:14px 16px; box-shadow:0 1px 3px rgba(0,0,0,.08); }
  .label { color:var(--muted); font-size:0.85rem; }
  .value { font-size:1.4rem; font-weight:600; margin-top:6px; word-break:break-all; }
  .unit { font-size:0.9rem; color:var(--muted); font-weight:400; }
  .pending .value { color:var(--muted); }
  footer { text-align:center; color:var(--muted); font-size:0.8rem; padding:12px; }
</style>
</head>
<body>
<header>
  <h1>Smart Orthopedic Splint</h1>
  <div class="sub"><span id="dot" class="dot"></span><span id="conn">Connecting...</span></div>
</header>
<main>
  <h2>Device</h2>
  <div class="grid">
    <div class="card"><div class="label">Hostname</div><div class="value" id="host">-</div></div>
    <div class="card"><div class="label">IP address</div><div class="value" id="ip">-</div></div>
    <div class="card"><div class="label">Wi-Fi signal</div><div class="value"><span id="rssi">-</span> <span class="unit">dBm</span></div></div>
    <div class="card"><div class="label">Uptime</div><div class="value" id="uptime">-</div></div>
    <div class="card"><div class="label">Free memory</div><div class="value"><span id="heap">-</span> <span class="unit">KB</span></div></div>
  </div>

  <h2>Sensors (not connected yet)</h2>
  <div class="grid">
    <div class="card pending"><div class="label">Pressure - hand (FSR 1)</div><div class="value">--</div></div>
    <div class="card pending"><div class="label">Pressure - wrist (FSR 2)</div><div class="value">--</div></div>
    <div class="card pending"><div class="label">Pressure - forearm (FSR 3)</div><div class="value">--</div></div>
    <div class="card pending"><div class="label">Skin temperature</div><div class="value">-- <span class="unit">&deg;C</span></div></div>
    <div class="card pending"><div class="label">Wrist angle</div><div class="value">-- <span class="unit">&deg;</span></div></div>
  </div>
</main>
<footer>Updates every 2 seconds</footer>
<script>
function fmtUptime(s) {
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return (h ? h + "h " : "") + m + "m " + sec + "s";
}
async function refresh() {
  try {
    const r = await fetch("/api/status", { cache: "no-store" });
    const d = await r.json();
    document.getElementById("host").textContent = d.hostname + ".local";
    document.getElementById("ip").textContent = d.ip;
    document.getElementById("rssi").textContent = d.rssi;
    document.getElementById("uptime").textContent = fmtUptime(d.uptime_s);
    document.getElementById("heap").textContent = (d.free_heap / 1024).toFixed(0);
    document.getElementById("dot").className = "dot on";
    document.getElementById("conn").textContent = "Connected to " + d.ssid;
  } catch (e) {
    document.getElementById("dot").className = "dot";
    document.getElementById("conn").textContent = "Device not reachable";
  }
}
refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
)HTML";

// ------------------------------------------------------------------ handlers
void handleRoot() {
  server.send_P(200, "text/html", PAGE);
}

void handleStatus() {
  String json = "{";
  json += "\"hostname\":\"" + String(HOSTNAME) + "\",";
  json += "\"ssid\":\"" + WiFi.SSID() + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime_s\":" + String(millis() / 1000) + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap());
  json += "}";
  server.send(200, "application/json", json);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ------------------------------------------------------------------ Wi-Fi
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);          // must be set before WiFi.begin()
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  if (REDUCE_TX_POWER) {
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
  }

  Serial.printf("Connecting to \"%s\"", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected");
    Serial.print("  IP address : ");
    Serial.println(WiFi.localIP());
    Serial.printf("  Signal     : %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("Wi-Fi connection failed - will retry. Check the SSID/password, and that the network is 2.4 GHz.");
  }
}

void startMDNS() {
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("  Open page  : http://%s.local  or  http://%s\n", HOSTNAME, WiFi.localIP().toString().c_str());
  } else {
    Serial.println("mDNS failed to start - use the IP address instead.");
  }
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  delay(1500);                         // give the USB serial port time to appear
  Serial.println("\nSmart Orthopedic Splint - Wi-Fi test");

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    startMDNS();
  }

  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started");
}

void loop() {
  server.handleClient();

  // check the connection every 10 seconds and reconnect if it dropped
  if (millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi lost - reconnecting...");
      WiFi.disconnect();
      connectWiFi();
      if (WiFi.status() == WL_CONNECTED) {
        MDNS.end();
        startMDNS();
      }
    }
  }
}
