/* SmartSplint All Sensors + Wi-Fi dashboard.
 * Based on the IMU-only sketch confirmed to read BOTH modules, combined
 * with the supplied working FSR/DS18B20 code and its GPIO3 temperature pin.
 * IMUs: SDA GPIO8, SCL GPIO9; AD0 GND (0x68), AD0 3V3 (0x69).
 * FSR1 GPIO0; FSR2 GPIO1; DS18B20 DATA GPIO3 (4.7k pull-up to 3V3).
 * Serial 115200; USB CDC On Boot ENABLED. r = reinitialize all; s = I2C scan.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Keep the working sensor wiring. Edit these network settings as needed.
const char *WIFI_SSID = "project";
const char *WIFI_PASSWORD = "12345678";
const char *HOSTNAME = "smartsplint";
const bool REDUCE_TX_POWER = false;
WebServer server(80);
bool wifiWasConnected = false, webStarted = false, samplesReady = false;
uint32_t lastWifiAttempt = 0;
// Cached sensor values: the HTTP handler never touches the I2C bus.
bool valid[2] = {false, false};
float accel[2][3] = {}, gyro[2][3] = {};
uint32_t readErrors[2] = {};

constexpr uint8_t SDA_PIN = 8;
constexpr uint8_t SCL_PIN = 9;
constexpr uint8_t ADDRESSES[2] = {0x68, 0x69};
// Some modules sold as MPU6050 report 0x98. This is an experimental register
// profile, NOT proof of compatibility. Check gravity and each axis response.
constexpr bool ALLOW_UNVERIFIED_0X98 = true;
bool ready[2] = {false, false};
uint8_t failures[2] = {};
uint32_t lastPrint = 0;

constexpr uint8_t FSR1_PIN = 0, FSR2_PIN = 1, TEMP_PIN = 3;
constexpr uint32_t SAMPLE_MS = 20, TEMP_MS = 1000;
OneWire oneWire(TEMP_PIN);
DallasTemperature thermometer(&oneWire);
DeviceAddress temperatureAddress;
bool temperatureFound = false, temperatureValid = false;
float temperatureC = NAN;
uint32_t conversionStarted = 0, lastSample = 0;
uint16_t fsrRaw[2] = {};
uint32_t fsrMv[2] = {};


bool readBytes(uint8_t address, uint8_t reg, uint8_t *data, uint8_t count) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  uint8_t error = Wire.endTransmission(false);
  if (error) {
    Serial.printf("0x%02X: register 0x%02X request failed, Wire error=%u\n", address, reg, error);
    return false;
  }
  size_t received = Wire.requestFrom(address, static_cast<size_t>(count), true);
  if (received != count) {
    Serial.printf("0x%02X: short read (%u of %u bytes)\n", address,
                  static_cast<unsigned>(received), count);
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t n = 0; n < count; ++n) data[n] = Wire.read();
  return true;
}

bool writeByte(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  uint8_t error = Wire.endTransmission();
  if (error) Serial.printf("0x%02X: write register 0x%02X failed, Wire error=%u\n", address, reg, error);
  return error == 0;
}

void scanBus() {
  Serial.printf("\nI2C pins: SDA=GPIO%u (%s), SCL=GPIO%u (%s)\n", SDA_PIN,
                digitalRead(SDA_PIN) ? "HIGH" : "LOW", SCL_PIN,
                digitalRead(SCL_PIN) ? "HIGH" : "LOW");
  Serial.println("Idle lines should normally be HIGH. Scanning addresses 0x01-0x7E...");
  uint8_t found = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();
    if (error == 0) {
      Serial.printf("  FOUND 0x%02X%s\n", address,
                    address == 0x68 ? " (AD0 LOW)" : address == 0x69 ? " (AD0 HIGH)" : "");
      ++found;
    }
    delay(1);
  }
  Serial.printf("Scan complete: %u responding address(es).\n", found);
  if (!found) Serial.println("No response: check 3V3/GND, SDA/SCL, header soldering and pull-ups.");
  Serial.println("One MPU: expect 0x68 OR 0x69. Two MPUs: expect BOTH.");
  Serial.println("Two devices with the same address cannot be distinguished by a scan.");
}

void initializeIMUs() {
  scanBus();
  for (uint8_t i = 0; i < 2; ++i) {
    ready[i] = false;
    valid[i] = false;
    failures[i] = 0;
    uint8_t address = ADDRESSES[i];
    Wire.beginTransmission(address);
    if (Wire.endTransmission() != 0) {
      Serial.printf("\n0x%02X: ABSENT (normal if testing only one module).\n", address);
      continue;
    }
    uint8_t identity;
    if (!readBytes(address, 0x75, &identity, 1)) continue;
    Serial.printf("\n0x%02X: WHO_AM_I=0x%02X\n", address, identity);
    bool newer = identity == 0x70 || identity == 0x71 || identity == 0x73;
    if (identity == 0x68) Serial.println("MPU6050 identity detected.");
    else if (newer) Serial.println("Alternative MPU65xx/92xx profile; verify the actual part and readings.");
    else if (identity == 0x98 && ALLOW_UNVERIFIED_0X98)
      Serial.println("WARNING: unverified 0x98 profile. Register/scale compatibility is NOT established.");
    else {
      Serial.println("Unsupported identity. Device responds, but will not be configured.");
      continue;
    }
    // WHO_AM_I on a genuine MPU6050 remains 0x68 even at bus address 0x69.
    bool ok = writeByte(address, 0x6B, 0x01); // wake; PLL clock
    delay(100);
    ok = writeByte(address, 0x6C, 0x00) && ok; // enable all axes
    ok = writeByte(address, 0x1A, 0x03) && ok; // gyro DLPF
    ok = writeByte(address, 0x19, 19) && ok;   // nominal 50 Hz
    ok = writeByte(address, 0x1B, 0x00) && ok; // +/-250 deg/s
    ok = writeByte(address, 0x1C, 0x00) && ok; // +/-2 g
    if (newer) ok = writeByte(address, 0x1D, 0x03) && ok;
    uint8_t power = 0xFF, gyroConfig = 0xFF, accelConfig = 0xFF;
    ok = readBytes(address, 0x6B, &power, 1) && ok;
    ok = readBytes(address, 0x1B, &gyroConfig, 1) && ok;
    ok = readBytes(address, 0x1C, &accelConfig, 1) && ok;
    Serial.printf("Read-back: PWR_MGMT_1=0x%02X GYRO_CONFIG=0x%02X ACCEL_CONFIG=0x%02X\n",
                  power, gyroConfig, accelConfig);
    ok = ok && power == 0x01 && gyroConfig == 0x00 && accelConfig == 0x00;
    ready[i] = ok;
    Serial.println(ok ? "READY" : "CONFIGURATION / READ-BACK FAILED");
  }
  Serial.println("\nCommands: r = rescan/reinitialize; s = scan only.\n");
}

int16_t signedWord(const uint8_t *p) {
  return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

void printIMU(uint8_t i) {
  valid[i] = false;
  if (!ready[i]) {
    Serial.printf("0x%02X: NOT READY (send r to retry)\n", ADDRESSES[i]);
    return;
  }
  uint8_t data[14];
  if (!readBytes(ADDRESSES[i], 0x3B, data, sizeof(data))) {
    ++readErrors[i];
    if (++failures[i] >= 3) {
      ready[i] = false;
      Serial.printf("0x%02X: stopped after 3 consecutive failures. Check wires, then send r.\n", ADDRESSES[i]);
    }
    return;
  }
  failures[i] = 0;
  float ax = signedWord(data) / 16384.0f;
  float ay = signedWord(data + 2) / 16384.0f;
  float az = signedWord(data + 4) / 16384.0f;
  float gx = signedWord(data + 8) / 131.0f;
  float gy = signedWord(data + 10) / 131.0f;
  float gz = signedWord(data + 12) / 131.0f;
  accel[i][0] = ax; accel[i][1] = ay; accel[i][2] = az;
  gyro[i][0] = gx; gyro[i][1] = gy; gyro[i][2] = gz;
  valid[i] = true;
  Serial.printf("0x%02X | A(g): X=%+.3f Y=%+.3f Z=%+.3f | G(deg/s): X=%+.2f Y=%+.2f Z=%+.2f\n",
                ADDRESSES[i], ax, ay, az, gx, gy, gz);
}


void initializeOtherSensors() {
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
}

void sampleFSRs() {
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

void printOtherSensors() {
  for (uint8_t i = 0; i < 2; ++i) {
    Serial.printf("FSR%u raw=%u/4095 | %lu mV%s\n", i + 1, fsrRaw[i],
                  static_cast<unsigned long>(fsrMv[i]),
                  fsrRaw[i] >= 4000 ? " | NEAR ADC LIMIT" : "");
  }
  if (temperatureValid) Serial.printf("DS18B20: %.2f C\n", temperatureC);
  else Serial.println(temperatureFound ? "DS18B20: waiting / invalid reading" : "DS18B20: NOT FOUND");
}

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
    <div class="card"><div class="label">DS18B20 temperature - GPIO3</div><div class="value" id="temperature">--</div><p id="temperatureState"></p></div>
  </div>
  <h2>Motion sensors</h2>
  <div class="grid">
    <div class="card"><div class="label">Forearm IMU - 0x68</div><p id="imuState0">Waiting</p><div id="imu0">--</div></div>
    <div class="card"><div class="label">Hand IMU - 0x69</div><p id="imuState1">Waiting</p><div id="imu1">--</div></div>
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
    json += ",\"status\":" + jsonString(!ready[i] ? "Not ready" : (valid[i] ? "OK" : "Read failed"));
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
  while (!Serial && millis() - started < 5000) delay(10);
  delay(500);
  Serial.println("\n=== SmartSplint ALL SENSORS + Wi-Fi v1 ===");
  Serial.println("FSR1=GPIO0; FSR2=GPIO1; DS18B20 DATA=GPIO3.");
  Serial.println("SDA GPIO8; SCL GPIO9. Use SDA/SCL pins, NOT XDA/XCL.");
  if (!Wire.begin(SDA_PIN, SCL_PIN)) {
    Serial.println("I2C controller initialization FAILED. Reset after checking board selection/pins.");
    while (true) delay(1000);
  }
  Wire.setClock(100000);
  Wire.setTimeOut(50);
  delay(200);
  analogReadResolution(12);
  analogSetPinAttenuation(FSR1_PIN, ADC_11db);
  analogSetPinAttenuation(FSR2_PIN, ADC_11db);
  initializeIMUs();
  initializeOtherSensors();
  sampleFSRs();
  printIMU(0);
  printIMU(1);
  startWeb();
}

void loop() {
  serviceWiFi();
  if (webStarted) server.handleClient();
  while (Serial.available()) {
    char command = Serial.read();
    if (command == 'r' || command == 'R') {
      initializeIMUs();
      initializeOtherSensors();
    }
    if (command == 's' || command == 'S') scanBus();
  }
  uint32_t now = millis();
  if (temperatureFound && now - conversionStarted >= TEMP_MS) {
    // 12-bit DS18B20 conversions need up to 750ms. No blocking wait.
    temperatureC = thermometer.getTempC(temperatureAddress);
    temperatureValid = temperatureC != DEVICE_DISCONNECTED_C
                       && temperatureC >= -55.0f && temperatureC <= 125.0f;
    thermometer.requestTemperaturesByAddress(temperatureAddress);
    conversionStarted = millis();
  }
  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    sampleFSRs();
  }
  if (now - lastPrint >= 500) {
    lastPrint = now;
    Serial.printf("\nTime: %lu ms\n", static_cast<unsigned long>(now));
    printIMU(0);
    printIMU(1);
    printOtherSensors();
  }
  delay(1);
}
