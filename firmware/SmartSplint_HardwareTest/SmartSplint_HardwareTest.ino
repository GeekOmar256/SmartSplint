/* Smart Splint bench test: ESP32-C3 SuperMini, 2 GY-521 MPU6050,
 * 2 FSR 400 dividers, 1 externally powered DS18B20.
 * Serial Monitor: 115200 baud. Send r to rescan/reinitialize.
 * Live dashboard: http://smartsplint.local or the IP printed in Serial.
 * Wi-Fi settings below. Wiring and expected results: see README.md beside this sketch.
 * Libraries: OneWire (Paul Stoffregen), DallasTemperature.
 * MPU6050 is accessed directly using the manufacturer's register map.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>

const char *WIFI_SSID = "project";
const char *WIFI_PASSWORD = "12345678";
const char *HOSTNAME = "smartsplint";
const bool REDUCE_TX_POWER = false;
// Experimental 0x98 support follows the supplied NoWiFi sketch; identity alone
// does not establish compatibility. Disable if measurements are implausible.
const bool ALLOW_UNVERIFIED_0X98 = true;
WebServer server(80);
bool wifiWasConnected = false, samplesReady = false, webStarted = false;
uint32_t lastWifiAttempt = 0;

constexpr uint8_t SDA_PIN = 8, SCL_PIN = 9;
constexpr uint8_t FSR1_PIN = 0, FSR2_PIN = 1, TEMP_PIN = 10;
constexpr uint32_t SAMPLE_MS = 20, PRINT_MS = 500, TEMP_MS = 1000;
constexpr uint8_t MPU_ADDR[2] = {0x68, 0x69};
OneWire oneWire(TEMP_PIN);
DallasTemperature thermometer(&oneWire);
DeviceAddress temperatureAddress;
bool temperatureFound = false, temperatureValid = false;
float temperatureC = NAN;
uint32_t conversionStarted = 0, lastSample = 0, lastPrint = 0;
bool initialized[2] = {false, false}, valid[2] = {false, false};
float accel[2][3] = {}, gyro[2][3] = {};
uint32_t readErrors[2] = {};
uint16_t fsrRaw[2] = {};
uint32_t fsrMv[2] = {};

bool readRegisters(uint8_t address, uint8_t reg, uint8_t *buffer, uint8_t count) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(address, static_cast<size_t>(count), true) != count) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < count; ++i) buffer[i] = Wire.read();
  return true;
}

bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

void scanBus() {
  Serial.println("I2C scan (expected: 0x68 AND 0x69):");
  uint8_t found = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Found 0x%02X\n", address);
      ++found;
    }
  }
  if (!found) Serial.println("  No I2C devices: check power, GND, SDA and SCL.");
}

void initializeSensors() {
  scanBus();
  for (uint8_t i = 0; i < 2; ++i) {
    valid[i] = initialized[i] = false;
    uint8_t identity = 0;
    if (!readRegisters(MPU_ADDR[i], 0x75, &identity, 1)) {
      Serial.printf("MPU %u at 0x%02X: NOT FOUND\n", i + 1, MPU_ADDR[i]);
      continue;
    }
    // Bus address 0x68/0x69 and WHO_AM_I are different concepts.
    const char *chip = identity == 0x68 ? "MPU6050"
                     : identity == 0x70 ? "MPU6500 register profile"
                     : identity == 0x71 ? "MPU9250 register profile"
                     : identity == 0x73 ? "MPU9255 register profile"
                     : identity == 0x98 && ALLOW_UNVERIFIED_0X98 ? "UNVERIFIED 0x98 profile" : nullptr;
    Serial.printf("MPU %u at 0x%02X WHO_AM_I=0x%02X: %s\n",
                  i + 1, MPU_ADDR[i], identity, chip ? chip : "UNSUPPORTED");
    if (!chip) continue;
    if (identity != 0x68) Serial.println("  Not a confirmed MPU6050. Verify axis response and scale before use.");
    bool ok = writeRegister(MPU_ADDR[i], 0x6B, 0x01); // wake, PLL clock
    delay(100);
    ok = writeRegister(MPU_ADDR[i], 0x6C, 0x00) && ok; // enable axes
    ok = writeRegister(MPU_ADDR[i], 0x1A, 0x03) && ok; // ~44 Hz DLPF
    ok = writeRegister(MPU_ADDR[i], 0x19, 19) && ok;   // 50 Hz
    ok = writeRegister(MPU_ADDR[i], 0x1B, 0x00) && ok; // +/-250 deg/s
    ok = writeRegister(MPU_ADDR[i], 0x1C, 0x00) && ok; // +/-2 g
    if (identity == 0x70 || identity == 0x71 || identity == 0x73)
      ok = writeRegister(MPU_ADDR[i], 0x1D, 0x03) && ok; // newer accel DLPF
    initialized[i] = ok;
    Serial.println(ok ? "  Configuration OK" : "  Configuration FAILED");
  }
  if (!initialized[0] || !initialized[1]) {
    Serial.println("DUAL IMU CHECK: both 0x68 and 0x69 must respond.");
    Serial.println("Forearm AD0 -> GND; hand AD0 -> 3V3. Both SDA -> GPIO8, SCL -> GPIO9.");
    Serial.println("If only 0x68 appears: test each board alone, then check AD0 wiring.");
  }
  thermometer.begin();
  temperatureFound = thermometer.getAddress(temperatureAddress, 0)
                     && temperatureAddress[0] == 0x28;
  temperatureValid = false;
  temperatureC = NAN;
  Serial.printf("1-Wire devices: %u; DS18B20: %s\n",
                thermometer.getDeviceCount(), temperatureFound ? "FOUND" : "NOT FOUND");
  if (temperatureFound) {
    Serial.print("DS18B20 ROM: ");
    for (uint8_t b : temperatureAddress) Serial.printf("%02X", b);
    Serial.println();
    thermometer.setResolution(temperatureAddress, 12);
    thermometer.setWaitForConversion(false);
    thermometer.requestTemperaturesByAddress(temperatureAddress);
    conversionStarted = millis();
  }
  Serial.println("Send r to rescan after reconnecting sensors.");
}

int16_t signedWord(const uint8_t *p) {
  return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

void sampleSensors() {
  for (uint8_t i = 0; i < 2; ++i) {
    valid[i] = false;
    if (!initialized[i]) continue;
    uint8_t bytes[14];
    if (!readRegisters(MPU_ADDR[i], 0x3B, bytes, sizeof(bytes))) {
      ++readErrors[i];
      continue;
    }
    for (uint8_t axis = 0; axis < 3; ++axis) {
      accel[i][axis] = signedWord(bytes + 2 * axis) / 16384.0f;
      gyro[i][axis] = signedWord(bytes + 8 + 2 * axis) / 131.0f;
    }
    valid[i] = true;
  }
  const uint8_t pins[2] = {FSR1_PIN, FSR2_PIN};
  for (uint8_t i = 0; i < 2; ++i) {
    uint32_t rawSum = 0, mvSum = 0;
    analogRead(pins[i]); // discard first reading after changing channels
    for (uint8_t n = 0; n < 16; ++n) {
      rawSum += analogRead(pins[i]);
      mvSum += analogReadMilliVolts(pins[i]);
    }
    fsrRaw[i] = rawSum / 16;
    fsrMv[i] = mvSum / 16;
  }
  samplesReady = true;
}

void printReadings() {
  Serial.printf("\nTime: %lu ms\n", static_cast<unsigned long>(millis()));
  for (uint8_t i = 0; i < 2; ++i) {
    if (valid[i]) {
      Serial.printf("MPU%u 0x%02X OK | accel g: X=%+.3f Y=%+.3f Z=%+.3f"
                    " | gyro deg/s: X=%+.2f Y=%+.2f Z=%+.2f\n",
                    i + 1, MPU_ADDR[i], accel[i][0], accel[i][1], accel[i][2],
                    gyro[i][0], gyro[i][1], gyro[i][2]);
    } else {
      Serial.printf("MPU%u 0x%02X %s | read errors=%lu\n", i + 1, MPU_ADDR[i],
                    initialized[i] ? "READ FAILED" : "NOT READY",
                    static_cast<unsigned long>(readErrors[i]));
    }
  }
  for (uint8_t i = 0; i < 2; ++i) {
    Serial.printf("FSR%u raw=%u/4095 | %lu mV%s\n", i + 1, fsrRaw[i],
                  static_cast<unsigned long>(fsrMv[i]),
                  fsrRaw[i] >= 4000 ? " | NEAR ADC LIMIT" : "");
  }
  if (temperatureValid) Serial.printf("DS18B20: %.2f C\n", temperatureC);
  else Serial.println(temperatureFound ? "DS18B20: waiting / invalid reading" : "DS18B20: NOT FOUND");
}


// Dashboard from the Wi-Fi trial, extended with live sensor snapshots.
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

  <h2>Live sensors</h2>
  <div class="grid">
    <div class="card"><div class="label">FSR 1   GPIO0</div><div class="value" id="fsr0">--</div><p id="fsrState0"></p></div>
    <div class="card"><div class="label">FSR 2   GPIO1</div><div class="value" id="fsr1">--</div><p id="fsrState1"></p></div>
    <div class="card"><div class="label">DS18B20 temperature</div><div class="value" id="temperature">--</div><p id="temperatureState"></p></div>
  </div>
  <h2>Motion sensors</h2>
  <div class="grid">
    <div class="card"><div class="label">Forearm MPU6050   0x68</div><p id="imuState0">Waiting</p><div id="imu0">--</div></div>
    <div class="card"><div class="label">Hand MPU6050   0x69</div><p id="imuState1">Waiting</p><div id="imu1">--</div></div>
  </div>
  <p class="sub">FSR readings are uncalibrated ADC counts and millivolts. Low readings cannot distinguish an unloaded FSR from a disconnected sensor. Motion readings are acceleration and angular velocity; wrist angles are not calculated.</p>
  <p class="sub">Reconnect a missing sensor, then send r in Serial Monitor to scan again.</p>
</main>
<footer>Updates every second   USB Serial: 115200 baud</footer>
<script>
function fmtUptime(s) {
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return (h ? h + "h " : "") + m + "m " + sec + "s";
}
const el = id => document.getElementById(id);
function clearSensors(message) {
  for (let i=0;i<2;i++) {
    el('fsr'+i).textContent='--'; el('fsrState'+i).textContent=message;
    el('imu'+i).textContent='--'; el('imuState'+i).textContent=message;
  }
  el('temperature').textContent='--'; el('temperatureState').textContent=message;
}
function renderSensors(d) {
  d.fsr.forEach((s,i) => {
    el('fsr'+i).textContent=s.raw===null ? '--' : s.raw+' / 4095';
    el('fsrState'+i).textContent=s.raw===null ? 'Waiting for sample' : s.mv+' mV'+(s.saturated?'   Near ADC limit':'   Uncalibrated');
  });
  el('temperature').textContent=d.temperature.c===null?'--':d.temperature.c.toFixed(2)+' \u00b0C';
  el('temperatureState').textContent=d.temperature.status;
  d.imu.forEach((m,i) => {
    el('imuState'+i).textContent=m.status+'   Read errors: '+m.read_errors;
    if (!m.ok) { el('imu'+i).textContent='--'; return; }
    const axes=v=>v.map((n,j)=>'XYZ'[j]+': '+n.toFixed(3)).join('  ');
    el('imu'+i).textContent='Acceleration (g): '+axes(m.accel_g)+' | Gyroscope (deg/s): '+axes(m.gyro_dps);
  });
}
async function refresh() {
  const controller=new AbortController();
  const timeout=setTimeout(()=>controller.abort(),4000);
  try {
    const r=await fetch('/api/status',{cache:'no-store',signal:controller.signal});
    if(!r.ok) throw new Error('HTTP '+r.status);
    const d=await r.json();
    el('host').textContent=d.hostname+'.local'; el('ip').textContent=d.ip;
    el('rssi').textContent=d.rssi; el('uptime').textContent=fmtUptime(d.uptime_s);
    el('heap').textContent=(d.free_heap/1024).toFixed(0);
    renderSensors(d);
    el('dot').className='dot on'; el('conn').textContent='Connected to '+d.ssid;
  } catch(e) {
    el('dot').className='dot'; el('conn').textContent='Device not reachable';
    clearSensors('No current data');
  } finally {
    clearTimeout(timeout); setTimeout(refresh,1000);
  }
}
refresh();
</script>
</body>
</html>
)HTML";


String jsonString(const String &input) {
  String result = "\"";
  for (size_t i = 0; i < input.length(); ++i) {
    uint8_t c = input[i];
    if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
    else if (c < 0x20) { char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", c); result += escaped; }
    else result += char(c);
  }
  return result + "\"";
}

void handleStatus() {
  String json;
  json.reserve(1500);
  json = "{\"hostname\":" + jsonString(HOSTNAME);
  json += ",\"ssid\":" + jsonString(WiFi.SSID());
  json += ",\"ip\":" + jsonString(WiFi.localIP().toString());
  json += ",\"rssi\":" + String(WiFi.RSSI());
  json += ",\"uptime_s\":" + String(millis() / 1000);
  json += ",\"free_heap\":" + String(ESP.getFreeHeap());
  json += ",\"fsr\":[";
  for (uint8_t i = 0; i < 2; ++i) {
    if (i) json += ',';
    json += "{\"raw\":" + (samplesReady ? String(fsrRaw[i]) : String("null"));
    json += ",\"mv\":" + (samplesReady ? String(fsrMv[i]) : String("null"));
    json += ",\"saturated\":";
    json += (samplesReady && fsrRaw[i] >= 4000) ? "true}" : "false}";
  }
  json += "],\"temperature\":{\"c\":";
  json += temperatureValid ? String(temperatureC, 2) : String("null");
  json += ",\"status\":";
  json += jsonString(!temperatureFound ? "Not found" : (temperatureValid ? "OK" : "Waiting / invalid reading"));
  json += "},\"imu\":[";
  for (uint8_t i = 0; i < 2; ++i) {
    if (i) json += ',';
    json += "{\"ok\":"; json += valid[i] ? "true" : "false";
    json += ",\"status\":" + jsonString(!initialized[i] ? "Not ready" : (valid[i] ? "OK" : "Read failed"));
    json += ",\"read_errors\":" + String(readErrors[i]);
    json += ",\"accel_g\":[";
    for (uint8_t axis = 0; axis < 3; ++axis) {
      if (axis) json += ',';
      json += valid[i] ? String(accel[i][axis], 4) : String("null");
    }
    json += "],\"gyro_dps\":[";
    for (uint8_t axis = 0; axis < 3; ++axis) {
      if (axis) json += ',';
      json += valid[i] ? String(gyro[i][axis], 4) : String("null");
    }
    json += "]}";
  }
  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void serviceWiFi() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiWasConnected) {
    Serial.printf("Wi-Fi connected. Open http://%s\n", WiFi.localIP().toString().c_str());
    // Start sockets only after Wi-Fi has initialized and obtained an IP.
    if (!webStarted) { server.begin(); webStarted = true; }
    MDNS.end();
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("Also available at http://%s.local\n", HOSTNAME);
    }
  }
  if (!connected && wifiWasConnected) {
    MDNS.end(); server.stop(); webStarted = false;
    Serial.println("Wi-Fi disconnected; HTTP server will restart after reconnection");
  }
  wifiWasConnected = connected;
  if (!connected && millis() - lastWifiAttempt >= 10000) {
    lastWifiAttempt = millis();
    Serial.println("Retrying Wi-Fi (sensor sampling continues)...");
    Serial.printf("Wi-Fi status=%d; reconnect requested\n", static_cast<int>(WiFi.status()));
    if (!WiFi.reconnect()) Serial.println("Reconnect request failed; check Wi-Fi event reason below.");
  }
}

void startWeb() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  // Do not call server.begin() here: TCP/IP may not be initialized yet.
  WiFi.persistent(false);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
      Serial.printf("Wi-Fi disconnect reason=%u\n", info.wifi_sta_disconnected.reason);
  });
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  if (REDUCE_TX_POWER) WiFi.setTxPower(WIFI_POWER_8_5dBm);
  lastWifiAttempt = millis();
  Serial.printf("Connecting to %s; sensors continue without Wi-Fi.\n", WIFI_SSID);
}

void setup() {
  Serial.begin(115200);
  uint32_t started = millis();
  while (!Serial && millis() - started < 4000) delay(10);
  Serial.println("\nSmart Splint - hardware test (2 MPU6050, 2 FSR, 1 DS18B20)");
  Serial.printf("Reset reason=%d (repeated power/brownout resets need a power check)\n", static_cast<int>(esp_reset_reason()));
  Serial.println("I2C: SDA GPIO8, SCL GPIO9. Keep the onboard LED unused.");
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(50);
  analogReadResolution(12);
  analogSetPinAttenuation(FSR1_PIN, ADC_11db);
  analogSetPinAttenuation(FSR2_PIN, ADC_11db);
  initializeSensors();
  startWeb();
}

void loop() {
  if (webStarted) server.handleClient();
  serviceWiFi();
  while (Serial.available()) {
    char command = Serial.read();
    if (command == 'r' || command == 'R') initializeSensors();
  }
  uint32_t now = millis();
  if (temperatureFound && now - conversionStarted >= TEMP_MS) {
    // Wait >=750 ms for a full 12-bit conversion; no blocking conversion delay.
    temperatureC = thermometer.getTempC(temperatureAddress);
    temperatureValid = temperatureC != DEVICE_DISCONNECTED_C
                       && temperatureC >= -55.0f && temperatureC <= 125.0f;
    thermometer.requestTemperaturesByAddress(temperatureAddress);
    conversionStarted = millis();
  }
  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    sampleSensors();
  }
  if (now - lastPrint >= PRINT_MS) {
    lastPrint = now;
    printReadings();
  }
  delay(1);
}
