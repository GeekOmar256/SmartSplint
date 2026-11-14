/* SmartSplint Monitor firmware - RAW DATA ONLY.
 *
 * The ESP32-C3 only acquires and sends data. All processing (scaling, gyro bias, filtering,
 * wrist angle, movement events, FSR calibration and analysis) is done by the SmartSplint Monitor
 * application on the computer.
 *
 * Every 20 ms (50 Hz) one sample is taken and stored in a ring buffer of 512 samples (about 10 s):
 *   seq, t_ms, forearm IMU ax ay az gx gy gz, hand IMU ax ay az gx gy gz (raw int16 counts),
 *   FSR1 raw, FSR2 raw (12-bit ADC counts), flags (bit0 forearm IMU ok, bit1 hand IMU ok)
 * The DS18B20 temperature is read once per second (the sensor itself reports degrees C).
 *
 * Two links to the computer (choose with a Serial command; the choice is saved in flash):
 *   Wi-Fi  : GET /api/samples?after=<seq>  samples newer than <seq> (max 250 per request) + header
 *            GET /api/status               device information and the latest sample
 *            GET /                         small device page for quick checks
 *   BLE    : device name "SmartSplint", service 6e7a0001-..., data characteristic 6e7a0002-... (notify).
 *            Each notification: 6-byte header + up to 5 samples of 37 bytes (little-endian):
 *              header : u8 version=1, u8 count, u8 status (bit0 forearm IMU ready, bit1 hand IMU ready,
 *                       bit2 temperature valid), i16 temperature x100 (deg C), u8 reserved
 *              sample : u32 seq, u32 t_ms, i16 forearm ax ay az gx gy gz, i16 hand ax ay az gx gy gz,
 *                       u16 fsr1, u16 fsr2, u8 flags
 *   BLE uses much less power than Wi-Fi. For battery use choose BLE only.
 *   Default link: BLE only (lowest power). Serial commands: w = Wi-Fi only, b = BLE only, a = both
 *   (the board restarts to apply and remembers the choice).
 *
 * Wiring (confirmed working):
 *   IMUs : SDA GPIO8, SCL GPIO9; forearm IMU AD0->GND (0x68), hand IMU AD0->3V3 (0x69)
 *   FSR1 : GPIO0, FSR2 : GPIO1   (3V3 -> FSR -> ADC pin -> 10k -> GND)
 *   DS18B20 DATA : GPIO3 with 4.7k pull-up to 3V3
 * Arduino IDE: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Libraries: OneWire, DallasTemperature.
 * Serial 115200. Commands: r = reinitialize sensors, s = I2C scan.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>

// ------------------------------------------------------------------ link selection (saved in flash)
enum LinkMode : uint8_t { LINK_WIFI = 1, LINK_BLE = 2, LINK_BOTH = 3 };
uint8_t linkMode = LINK_BLE;     // default: Bluetooth only
Preferences prefs;

// ------------------------------------------------------------------ BLE
const char *BLE_NAME = "SmartSplint";
#define SERVICE_UUID "6e7a0001-5a3b-4c8e-9f21-7d4a2c1b0e01"
#define DATA_UUID    "6e7a0002-5a3b-4c8e-9f21-7d4a2c1b0e01"
BLEServer *bleServer = nullptr;
BLECharacteristic *bleData = nullptr;
volatile bool bleConnected = false;
volatile bool bleJustConnected = false;
uint16_t bleConnId = 0;
uint32_t bleSentSeq = 0, lastBleSend = 0;

// ------------------------------------------------------------------ network
const char *WIFI_SSID = "project";
const char *WIFI_PASSWORD = "12345678";
const char *HOSTNAME = "smartsplint";
const bool REDUCE_TX_POWER = false;
WebServer server(80);
bool wifiWasConnected = false, webStarted = false;
uint32_t lastWifiAttempt = 0;

// ------------------------------------------------------------------ pins and timing
constexpr uint8_t SDA_PIN = 8, SCL_PIN = 9;
constexpr uint8_t ADDRESSES[2] = {0x68, 0x69};   // [0] forearm, [1] hand
constexpr bool ALLOW_UNVERIFIED_0X98 = true;      // kept from the working sketch
constexpr uint8_t FSR_PINS[2] = {0, 1};
constexpr uint8_t TEMP_PIN = 3;
constexpr uint32_t SAMPLE_MS = 20;                // 50 Hz
constexpr uint32_t TEMP_MS = 1000;
constexpr uint16_t BUFFER_SIZE = 512;
constexpr uint16_t MAX_ROWS_PER_REQUEST = 250;

// ------------------------------------------------------------------ ring buffer
struct Sample {
  uint32_t seq;
  uint32_t t;
  int16_t imu[2][6];
  uint16_t fsr[2];
  uint8_t flags;
};
Sample buffer[BUFFER_SIZE];
uint32_t nextSeq = 1;          // seq of the next sample to be written
uint32_t lastSampleTime = 0;

bool imuReady[2] = {false, false};
uint8_t imuWho[2] = {0, 0};
uint8_t imuFailures[2] = {0, 0};
uint32_t imuReadErrors[2] = {0, 0};
uint32_t lastImuRetry = 0;

OneWire oneWire(TEMP_PIN);
DallasTemperature thermometer(&oneWire);
DeviceAddress temperatureAddress;
bool temperatureFound = false, temperatureValid = false;
float temperatureC = NAN;
uint32_t conversionStarted = 0;

// ================================================================== I2C
bool readBytes(uint8_t address, uint8_t reg, uint8_t *data, uint8_t count) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false)) return false;
  size_t received = Wire.requestFrom(address, static_cast<size_t>(count), true);
  if (received != count) {
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
  return Wire.endTransmission() == 0;
}

void scanBus() {
  Serial.println("\nI2C scan (SDA GPIO8, SCL GPIO9):");
  uint8_t found = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  FOUND 0x%02X\n", address);
      ++found;
    }
    delay(1);
  }
  Serial.printf("Scan complete: %u device(s). Two IMUs: expect 0x68 AND 0x69.\n", found);
}

bool initIMU(uint8_t i, bool verbose) {
  imuReady[i] = false;
  imuFailures[i] = 0;
  uint8_t address = ADDRESSES[i];
  Wire.beginTransmission(address);
  if (Wire.endTransmission() != 0) {
    if (verbose) Serial.printf("0x%02X: ABSENT\n", address);
    return false;
  }
  uint8_t id = 0;
  if (!readBytes(address, 0x75, &id, 1)) return false;
  imuWho[i] = id;
  bool newer = id == 0x70 || id == 0x71 || id == 0x73;
  bool supported = id == 0x68 || newer || (id == 0x98 && ALLOW_UNVERIFIED_0X98);
  if (verbose) Serial.printf("0x%02X: WHO_AM_I=0x%02X%s\n", address, id, supported ? "" : " (unsupported)");
  if (!supported) return false;
  bool ok = writeByte(address, 0x6B, 0x01);   // wake, PLL clock
  delay(100);
  ok = writeByte(address, 0x6C, 0x00) && ok;  // all axes on
  ok = writeByte(address, 0x1A, 0x03) && ok;  // sensor's own low-pass filter ~44 Hz (anti-aliasing)
  ok = writeByte(address, 0x19, 4) && ok;     // 200 Hz internal rate
  ok = writeByte(address, 0x1B, 0x00) && ok;  // +/-250 deg/s -> 131 LSB per deg/s
  ok = writeByte(address, 0x1C, 0x00) && ok;  // +/-2 g      -> 16384 LSB per g
  if (newer) ok = writeByte(address, 0x1D, 0x03) && ok;
  uint8_t power = 0xFF, gyroConfig = 0xFF, accelConfig = 0xFF;
  ok = readBytes(address, 0x6B, &power, 1) && ok;
  ok = readBytes(address, 0x1B, &gyroConfig, 1) && ok;
  ok = readBytes(address, 0x1C, &accelConfig, 1) && ok;
  ok = ok && power == 0x01 && gyroConfig == 0x00 && accelConfig == 0x00;
  imuReady[i] = ok;
  if (verbose) Serial.printf("0x%02X: %s\n", address, ok ? "READY" : "CONFIGURATION FAILED");
  return ok;
}

bool readImuRaw(uint8_t i, int16_t out[6]) {
  uint8_t d[14];
  if (!readBytes(ADDRESSES[i], 0x3B, d, sizeof(d))) return false;
  // accel X,Y,Z = bytes 0-5, temperature = 6-7 (skipped), gyro X,Y,Z = 8-13
  const uint8_t offsets[6] = {0, 2, 4, 8, 10, 12};
  for (uint8_t k = 0; k < 6; ++k)
    out[k] = static_cast<int16_t>((static_cast<uint16_t>(d[offsets[k]]) << 8) | d[offsets[k] + 1]);
  return true;
}

// ================================================================== acquisition
void takeSample() {
  Sample &s = buffer[nextSeq % BUFFER_SIZE];
  s.seq = nextSeq;
  s.t = millis();
  s.flags = 0;
  for (uint8_t i = 0; i < 2; ++i) {
    bool ok = imuReady[i] && readImuRaw(i, s.imu[i]);
    if (ok) {
      imuFailures[i] = 0;
      s.flags |= (1 << i);
    } else {
      for (uint8_t k = 0; k < 6; ++k) s.imu[i][k] = 0;
      if (imuReady[i]) {
        ++imuReadErrors[i];
        if (++imuFailures[i] >= 10) {
          imuReady[i] = false;
          Serial.printf("0x%02X: stopped after repeated read failures (auto-retry every 5 s)\n", ADDRESSES[i]);
        }
      }
    }
  }
  for (uint8_t i = 0; i < 2; ++i) {
    analogRead(FSR_PINS[i]);                // discard the first conversion after switching channel
    s.fsr[i] = analogRead(FSR_PINS[i]);
  }
  ++nextSeq;
}

void initializeTemperature() {
  thermometer.begin();
  temperatureFound = thermometer.getAddress(temperatureAddress, 0) && temperatureAddress[0] == 0x28;
  temperatureValid = false;
  temperatureC = NAN;
  Serial.printf("DS18B20: %s\n", temperatureFound ? "FOUND" : "NOT FOUND");
  if (temperatureFound) {
    thermometer.setResolution(temperatureAddress, 12);
    thermometer.setWaitForConversion(false);
    thermometer.requestTemperaturesByAddress(temperatureAddress);
    conversionStarted = millis();
  }
}

void initializeAll() {
  scanBus();
  for (uint8_t i = 0; i < 2; ++i) initIMU(i, true);
  initializeTemperature();
}

// ================================================================== JSON
String jsonString(const String &input) {
  String result = "\"";
  for (size_t i = 0; i < input.length(); ++i) {
    uint8_t c = input[i];
    if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
    else if (c < 0x20) { char e[7]; snprintf(e, sizeof(e), "\\u%04x", c); result += e; }
    else result += char(c);
  }
  return result + "\"";
}

void appendHeader(String &j) {
  j += "\"firmware\":\"SmartSplint_Monitor_FW 2.0 (raw)\"";
  j += ",\"hostname\":" + jsonString(HOSTNAME);
  j += ",\"ip\":" + jsonString(WiFi.localIP().toString());
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"uptime_ms\":" + String(millis());
  j += ",\"free_heap\":" + String(ESP.getFreeHeap());
  j += ",\"rate_hz\":" + String(1000 / SAMPLE_MS);
  j += ",\"accel_lsb_per_g\":16384,\"gyro_lsb_per_dps\":131,\"adc_bits\":12";
  j += ",\"imu_ready\":[";
  j += imuReady[0] ? "true," : "false,";
  j += imuReady[1] ? "true]" : "false]";
  j += ",\"imu_who_am_i\":[" + String(imuWho[0]) + "," + String(imuWho[1]) + "]";
  j += ",\"imu_read_errors\":[" + String(imuReadErrors[0]) + "," + String(imuReadErrors[1]) + "]";
  j += ",\"temp_found\":";
  j += temperatureFound ? "true" : "false";
  j += ",\"temp_c\":" + (temperatureValid ? String(temperatureC, 2) : String("null"));
  j += ",\"fields\":[\"seq\",\"t_ms\",\"f_ax\",\"f_ay\",\"f_az\",\"f_gx\",\"f_gy\",\"f_gz\","
       "\"h_ax\",\"h_ay\",\"h_az\",\"h_gx\",\"h_gy\",\"h_gz\",\"fsr1\",\"fsr2\",\"flags\"]";
}

void appendRow(String &j, const Sample &s) {
  char line[160];
  snprintf(line, sizeof(line), "[%lu,%lu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u]",
           (unsigned long)s.seq, (unsigned long)s.t,
           s.imu[0][0], s.imu[0][1], s.imu[0][2], s.imu[0][3], s.imu[0][4], s.imu[0][5],
           s.imu[1][0], s.imu[1][1], s.imu[1][2], s.imu[1][3], s.imu[1][4], s.imu[1][5],
           s.fsr[0], s.fsr[1], s.flags);
  j += line;
}

void handleSamples() {
  uint32_t newest = nextSeq - 1;
  uint32_t oldest = nextSeq > BUFFER_SIZE ? nextSeq - BUFFER_SIZE : 1;
  uint32_t after = server.hasArg("after") ? strtoul(server.arg("after").c_str(), nullptr, 10) : 0;
  uint32_t first = max(after + 1, oldest);
  if (after > newest) first = oldest;        // the device restarted: send what we have
  uint32_t last = min(newest, first + MAX_ROWS_PER_REQUEST - 1);
  String j;
  j.reserve(400 + (last >= first ? (last - first + 1) * 90 : 0));
  j = "{";
  appendHeader(j);
  j += ",\"oldest_seq\":" + String(oldest);
  j += ",\"newest_seq\":" + String(newest);
  j += ",\"rows\":[";
  for (uint32_t seq = first; newest >= 1 && seq <= last; ++seq) {
    if (seq != first) j += ',';
    appendRow(j, buffer[seq % BUFFER_SIZE]);
  }
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", j);
}

void handleStatus() {
  String j = "{";
  appendHeader(j);
  j += ",\"latest\":";
  if (nextSeq > 1) appendRow(j, buffer[(nextSeq - 1) % BUFFER_SIZE]);
  else j += "null";
  j += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", j);
}

const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Smart Splint device</title>
<style>body{font-family:system-ui,sans-serif;margin:16px;background:#f4f6f8;color:#1d2733}
.c{background:#fff;border-radius:10px;padding:12px;margin:8px 0}code{font-size:.9rem}</style></head>
<body><h2>Smart Splint - raw data device</h2><p id="s">Connecting...</p><div class="c"><code id="r">--</code></div>
<p>This device sends raw data only. Open the <b>SmartSplint Monitor</b> application on the computer for calibration,
charts and analysis.</p>
<script>
const $=i=>document.getElementById(i);
async function tick(){try{const d=await(await fetch('/api/status',{cache:'no-store'})).json();
$('s').textContent='Connected - '+d.ip+' - IMUs ready: '+d.imu_ready.join(', ')+' - temperature: '+(d.temp_c??'--')+' C';
$('r').textContent=d.latest?d.fields.map((f,i)=>f+'='+d.latest[i]).join('  '):'no samples yet'}
catch(e){$('s').textContent='Device not reachable'}finally{setTimeout(tick,1000)}}tick();
</script></body></html>)HTML";

// ================================================================== Wi-Fi
void serviceWiFi() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiWasConnected) {
    Serial.printf("Wi-Fi connected. Device address: http://%s  (or http://%s.local)\n",
                  WiFi.localIP().toString().c_str(), HOSTNAME);
    if (!webStarted) { server.begin(); webStarted = true; }
    MDNS.end();
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  }
  if (!connected && wifiWasConnected) {
    MDNS.end();
    server.stop();
    webStarted = false;
    Serial.println("Wi-Fi disconnected; sampling continues (buffer holds about 10 s)");
  }
  wifiWasConnected = connected;
  if (!connected && millis() - lastWifiAttempt >= 10000) {
    lastWifiAttempt = millis();
    WiFi.reconnect();
  }
}

void startWeb() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/api/samples", HTTP_GET, handleSamples);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  WiFi.persistent(false);
  WiFi.setHostname(HOSTNAME);   // before mode/begin so the router also sees the name
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  if (REDUCE_TX_POWER) WiFi.setTxPower(WIFI_POWER_8_5dBm);
  lastWifiAttempt = millis();
  Serial.printf("Connecting to %s...\n", WIFI_SSID);
}

// ================================================================== BLE
class ServerCallbacks : public BLEServerCallbacks {
  // Use the shared callback API: the two-argument form depends on the BLE
  // backend (Bluedroid vs NimBLE) selected by the installed ESP32 core.
  void onConnect(BLEServer *server) override {
    bleConnId = server->getConnId();
    bleConnected = true;
    bleJustConnected = true;
  }
  void onDisconnect(BLEServer *server) override {
    bleConnected = false;
    BLEDevice::startAdvertising();   // allow the computer to reconnect
  }
};

void startBLE() {
  BLEDevice::init(BLE_NAME);
  BLEDevice::setMTU(247);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  BLEService *service = bleServer->createService(SERVICE_UUID);
  bleData = service->createCharacteristic(DATA_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  bleData->addDescriptor(new BLE2902());
  service->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("BLE advertising as \"%s\"\n", BLE_NAME);
}

void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(uint8_t *p, uint32_t v) { for (uint8_t k = 0; k < 4; ++k) p[k] = (v >> (8 * k)) & 0xFF; }

// Send buffered samples as notifications (up to 5 samples per packet, every 100 ms or sooner).
void serviceBLE() {
  if (!bleConnected || !bleData) return;
  uint32_t newest = nextSeq - 1;
  if (bleJustConnected) {             // start from the current sample after (re)connection
    bleJustConnected = false;
    bleSentSeq = newest;
    return;
  }
  uint16_t mtu = bleServer->getPeerMTU(bleConnId);
  uint8_t perPacket = mtu > 9 ? min<uint16_t>(5, (mtu - 3 - 6) / 37) : 0;
  if (perPacket == 0) return;         // MTU exchange not finished yet
  uint32_t oldest = nextSeq > BUFFER_SIZE ? nextSeq - BUFFER_SIZE : 1;
  if (bleSentSeq + 1 < oldest) bleSentSeq = oldest - 1;
  uint32_t pending = newest - bleSentSeq;
  if (pending == 0 || (pending < perPacket && millis() - lastBleSend < 100)) return;
  uint8_t packet[6 + 5 * 37];
  uint8_t count = min<uint32_t>(pending, perPacket);
  packet[0] = 1;
  packet[1] = count;
  packet[2] = (imuReady[0] ? 1 : 0) | (imuReady[1] ? 2 : 0) | (temperatureValid ? 4 : 0);
  put16(packet + 3, static_cast<uint16_t>(temperatureValid ? static_cast<int16_t>(lroundf(temperatureC * 100)) : 0x7FFF));
  packet[5] = 0;
  uint8_t *p = packet + 6;
  for (uint8_t n = 0; n < count; ++n) {
    const Sample &s = buffer[(bleSentSeq + 1 + n) % BUFFER_SIZE];
    put32(p, s.seq);
    put32(p + 4, s.t);
    for (uint8_t i = 0; i < 2; ++i)
      for (uint8_t k = 0; k < 6; ++k) put16(p + 8 + 12 * i + 2 * k, static_cast<uint16_t>(s.imu[i][k]));
    put16(p + 32, s.fsr[0]);
    put16(p + 34, s.fsr[1]);
    p[36] = s.flags;
    p += 37;
  }
  bleData->setValue(packet, 6 + count * 37);
  bleData->notify();
  bleSentSeq += count;
  lastBleSend = millis();
}

void setLinkMode(uint8_t mode) {
  prefs.putUChar("link", mode);
  Serial.printf("Link set to %s. Restarting...\n",
                mode == LINK_WIFI ? "Wi-Fi only" : mode == LINK_BLE ? "BLE only" : "Wi-Fi + BLE");
  delay(300);
  ESP.restart();
}

// ================================================================== setup / loop
void setup() {
  Serial.begin(115200);
  uint32_t started = millis();
  while (!Serial && millis() - started < 2000) delay(10);
  Serial.println("\n=== SmartSplint Monitor firmware 2.0 (raw data) ===");
  if (!Wire.begin(SDA_PIN, SCL_PIN)) {
    Serial.println("I2C initialization FAILED");
    while (true) delay(1000);
  }
  Wire.setClock(100000);
  Wire.setTimeOut(50);
  analogReadResolution(12);
  for (uint8_t i = 0; i < 2; ++i) analogSetPinAttenuation(FSR_PINS[i], ADC_11db);
  initializeAll();
  prefs.begin("splint", false);
  linkMode = prefs.getUChar("link", LINK_BLE);
  if (linkMode < LINK_WIFI || linkMode > LINK_BOTH) linkMode = LINK_BLE;
  Serial.printf("Link: %s   (Serial: w = Wi-Fi only, b = BLE only, a = both)\n",
                linkMode == LINK_WIFI ? "Wi-Fi only" : linkMode == LINK_BLE ? "BLE only" : "Wi-Fi + BLE");
  if (linkMode & LINK_WIFI) startWeb();
  else WiFi.mode(WIFI_OFF);
  if (linkMode & LINK_BLE) startBLE();
  lastSampleTime = millis();
}

void loop() {
  if (linkMode & LINK_WIFI) {
    serviceWiFi();
    if (webStarted) server.handleClient();
  }
  if (linkMode & LINK_BLE) serviceBLE();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'r' || c == 'R') initializeAll();
    if (c == 's' || c == 'S') scanBus();
    if (c == 'w' || c == 'W') setLinkMode(LINK_WIFI);
    if (c == 'b' || c == 'B') setLinkMode(LINK_BLE);
    if (c == 'a' || c == 'A') setLinkMode(LINK_BOTH);
  }

  uint32_t now = millis();
  if (now - lastSampleTime >= SAMPLE_MS) {
    lastSampleTime += SAMPLE_MS;
    if (now - lastSampleTime > 200) lastSampleTime = now;   // no catch-up burst after a long pause
    takeSample();
  }
  if (now - lastImuRetry >= 5000) {
    lastImuRetry = now;
    for (uint8_t i = 0; i < 2; ++i)
      if (!imuReady[i] && initIMU(i, false)) Serial.printf("0x%02X: reconnected\n", ADDRESSES[i]);
    // once every 5 s: short status line on the serial monitor
    Sample &s = buffer[(nextSeq - 1) % BUFFER_SIZE];
    Serial.printf("seq %lu | IMU ready %d %d | FSR raw %u %u | T %s C\n", (unsigned long)(nextSeq - 1),
                  imuReady[0], imuReady[1], s.fsr[0], s.fsr[1],
                  temperatureValid ? String(temperatureC, 2).c_str() : "--");
  }
  if (temperatureFound && now - conversionStarted >= TEMP_MS) {
    temperatureC = thermometer.getTempC(temperatureAddress);
    temperatureValid = temperatureC != DEVICE_DISCONNECTED_C && temperatureC >= -55.0f && temperatureC <= 125.0f;
    thermometer.requestTemperaturesByAddress(temperatureAddress);
    conversionStarted = millis();
  }
}
