/* SmartSplint Monitor BLE-only firmware - raw data.
 * Device name: SmartSplint. Same BLE UUIDs and version-1 packet format as
 * SmartSplint_Monitor_FW, compatible with the computer application's BLE link.
 * Two IMUs: SDA GPIO8 / SCL GPIO9; AD0 GND (0x68) and 3V3 (0x69).
 * FSR1 GPIO0, FSR2 GPIO1; DS18B20 DATA GPIO3, 4.7k pull-up to 3V3.
 * IMU and FSR acquisition: nominal 50 Hz; temperature: about 1 Hz.
 * Serial 115200: r = reinitialize all sensors, s = scan I2C.
 * Arduino: ESP32C3 Dev Module, USB CDC On Boot enabled.
 * External libraries: OneWire, DallasTemperature. BLE is bundled with ESP32.
 * No Wi-Fi library, web server, credentials, or saved link-mode selection.
 */
#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>

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

// ------------------------------------------------------------------ pins and timing
constexpr uint8_t SDA_PIN = 8, SCL_PIN = 9;
constexpr uint8_t ADDRESSES[2] = {0x68, 0x69};   // [0] forearm, [1] hand
constexpr bool ALLOW_UNVERIFIED_0X98 = true;      // kept from the working sketch
constexpr uint8_t FSR_PINS[2] = {0, 1};
constexpr uint8_t TEMP_PIN = 3;
constexpr uint32_t SAMPLE_MS = 20;                // 50 Hz
constexpr uint32_t TEMP_MS = 1000;
constexpr uint16_t BUFFER_SIZE = 512;

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
uint32_t lastImuPrint = 0;

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
  ok = writeByte(address, 0x19, 19) && ok;    // 50 Hz, same as working reference
  ok = writeByte(address, 0x1B, 0x00) && ok;  // +/-250 deg/s -> 131 LSB per deg/s
  ok = writeByte(address, 0x1C, 0x00) && ok;  // +/-2 g      -> 16384 LSB per g
  if (newer) ok = writeByte(address, 0x1D, 0x03) && ok;
  uint8_t power = 0xFF, gyroConfig = 0xFF, accelConfig = 0xFF;
  ok = readBytes(address, 0x6B, &power, 1) && ok;
  ok = readBytes(address, 0x1B, &gyroConfig, 1) && ok;
  ok = readBytes(address, 0x1C, &accelConfig, 1) && ok;
  if (verbose) Serial.printf("Read-back 0x%02X: PWR=0x%02X GYRO=0x%02X ACCEL=0x%02X\n",
                            address, power, gyroConfig, accelConfig);
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

// Show the same units as the working Wi-Fi reference, without changing the
// raw counts sent to the app (which performs the unit conversion once).
void printImuDiagnostics() {
  if (nextSeq <= 1) return;
  const Sample &s = buffer[(nextSeq - 1) % BUFFER_SIZE];
  for (uint8_t i = 0; i < 2; ++i) {
    if (!(s.flags & (1 << i))) {
      Serial.printf("0x%02X | NO VALID SAMPLE | ready=%d WHO=0x%02X read errors=%lu\n",
                    ADDRESSES[i], imuReady[i], imuWho[i], (unsigned long)imuReadErrors[i]);
      continue;
    }
    Serial.printf("0x%02X | A(g): X=%+.3f Y=%+.3f Z=%+.3f | G(deg/s): X=%+.2f Y=%+.2f Z=%+.2f\n",
                  ADDRESSES[i], s.imu[i][0] / 16384.0f, s.imu[i][1] / 16384.0f,
                  s.imu[i][2] / 16384.0f, s.imu[i][3] / 131.0f,
                  s.imu[i][4] / 131.0f, s.imu[i][5] / 131.0f);
  }
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

// ================================================================== setup / loop
void setup() {
  Serial.begin(115200);
  uint32_t started = millis();
  while (!Serial && millis() - started < 2000) delay(10);
  delay(500);
  Serial.println("\n=== SmartSplint Monitor BLE ONLY 1.1 (reference IMU settings) ===");
  if (!Wire.begin(SDA_PIN, SCL_PIN)) {
    Serial.println("I2C initialization FAILED");
    while (true) delay(1000);
  }
  Wire.setClock(100000);
  Wire.setTimeOut(50);
  delay(200);  // same sensor startup settling time as the working reference
  analogReadResolution(12);
  for (uint8_t i = 0; i < 2; ++i) analogSetPinAttenuation(FSR_PINS[i], ADC_11db);
  initializeAll();
  Serial.println("Bluetooth only. Serial commands: r = reinitialize sensors; s = I2C scan.");
  startBLE();
  lastSampleTime = millis();
}

void loop() {
  serviceBLE();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'r' || c == 'R') initializeAll();
    if (c == 's' || c == 'S') scanBus();
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
    uint16_t mtu = bleConnected ? bleServer->getPeerMTU(bleConnId) : 0;
    Serial.printf("BLE connected=%d MTU=%u last sent seq=%lu latest seq=%lu\n",
                  bleConnected, mtu, (unsigned long)bleSentSeq, (unsigned long)(nextSeq - 1));
    if (bleConnected && mtu < 46)
      Serial.println("BLE MTU too small for one sample; sensor data may be valid but cannot reach the app.");
  }
  if (now - lastImuPrint >= 500) {
    lastImuPrint = now;
    printImuDiagnostics();
  }
  if (temperatureFound && now - conversionStarted >= TEMP_MS) {
    temperatureC = thermometer.getTempC(temperatureAddress);
    temperatureValid = temperatureC != DEVICE_DISCONNECTED_C && temperatureC >= -55.0f && temperatureC <= 125.0f;
    thermometer.requestTemperaturesByAddress(temperatureAddress);
    conversionStarted = millis();
  }
}
