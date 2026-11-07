/* SmartSplint All Sensors - NO Wi-Fi.
 * Based on the IMU-only sketch confirmed to read BOTH modules, combined
 * with the supplied working FSR/DS18B20 code and its GPIO3 temperature pin.
 * IMUs: SDA GPIO8, SCL GPIO9; AD0 GND (0x68), AD0 3V3 (0x69).
 * FSR1 GPIO0; FSR2 GPIO1; DS18B20 DATA GPIO3 (4.7k pull-up to 3V3).
 * Serial 115200; USB CDC On Boot ENABLED. r = reinitialize all; s = I2C scan.
 */
#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>

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
  if (!ready[i]) {
    Serial.printf("0x%02X: NOT READY (send r to retry)\n", ADDRESSES[i]);
    return;
  }
  uint8_t data[14];
  if (!readBytes(ADDRESSES[i], 0x3B, data, sizeof(data))) {
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

void setup() {
  Serial.begin(115200);
  uint32_t started = millis();
  while (!Serial && millis() - started < 5000) delay(10);
  delay(500);
  Serial.println("\n=== SmartSplint ALL SENSORS v1 - NO Wi-Fi ===");
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
}

void loop() {
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
