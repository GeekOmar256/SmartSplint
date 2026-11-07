/* ESP32-C3 SuperMini: IMU-only diagnostic (no Wi-Fi or other sensors).
 * Both modules: VCC -> 3V3, GND -> GND, SDA -> GPIO8, SCL -> GPIO9.
 * First AD0 -> GND (0x68); second AD0 -> 3V3 (0x69).
 * A single module can be tested at either address. No external libraries.
 * Serial Monitor 115200; USB CDC On Boot ENABLED. Send r to rescan.
 */
#include <Arduino.h>
#include <Wire.h>

constexpr uint8_t SDA_PIN = 8;
constexpr uint8_t SCL_PIN = 9;
constexpr uint8_t ADDRESSES[2] = {0x68, 0x69};
// Some modules sold as MPU6050 report 0x98. This is an experimental register
// profile, NOT proof of compatibility. Check gravity and each axis response.
constexpr bool ALLOW_UNVERIFIED_0X98 = true;
bool ready[2] = {false, false};
uint8_t failures[2] = {};
uint32_t lastPrint = 0;

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
  if (!ready[i]) return;
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

void setup() {
  Serial.begin(115200);
  uint32_t started = millis();
  while (!Serial && millis() - started < 5000) delay(10);
  delay(500);
  Serial.println("\n=== SmartSplint IMU ONLY v1 ===");
  Serial.println("No Wi-Fi, FSR or DS18B20. Disconnect those components for isolation.");
  Serial.println("SDA GPIO8; SCL GPIO9. Use SDA/SCL pins, NOT XDA/XCL.");
  if (!Wire.begin(SDA_PIN, SCL_PIN)) {
    Serial.println("I2C controller initialization FAILED. Reset after checking board selection/pins.");
    while (true) delay(1000);
  }
  Wire.setClock(100000);
  Wire.setTimeOut(50);
  delay(200);
  initializeIMUs();
}

void loop() {
  while (Serial.available()) {
    char command = Serial.read();
    if (command == 'r' || command == 'R') initializeIMUs();
    if (command == 's' || command == 'S') scanBus();
  }
  if (millis() - lastPrint >= 500) {
    lastPrint = millis();
    if (ready[0] || ready[1]) {
      printIMU(0);
      printIMU(1);
    } else Serial.println("No IMU ready. Fix connections, then send r to retry.");
  }
  delay(1);
}
