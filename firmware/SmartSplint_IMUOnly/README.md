# IMU-only isolation test

Open `SmartSplint_IMUOnly.ino`. Select **ESP32C3 Dev Module**, **USB CDC On Boot: Enabled**, then upload. Open Serial Monitor at **115200 baud** and press RESET. No external libraries are required.

Disconnect Wi-Fi-test extras (FSRs, temperature sensor and battery/boost module) with power off. Use USB bench power. First connect just one GY-521:

| GY-521 pin | ESP32-C3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO8 |
| SCL | GPIO9 |
| AD0 | GND |
| XDA, XCL, INT | Unconnected |

Use GPIO labels, not physical header positions. The SuperMini LED shares GPIO8; this sketch never drives it. Do not apply 5 V to the GPIOs. The I2C lines need pull-ups to 3V3; GY-521 boards normally include them. If absent, add approximately 4.7 kOhm from SDA to 3V3 and another from SCL to 3V3. Keep wires short and verify header solder joints.

1. With one module and AD0 grounded, expect **FOUND 0x68**, an identity, and **READY**. ABSENT at 0x69 is expected.
2. Hold it still and flat: one acceleration axis should be near +1 or -1 g, with the other two near zero. Gyro readings should be near zero with some uncalibrated bias. Rotate it and check all axes respond.
3. Power off and test the second module alone with the same wires. Repeat step 2.
4. Power off and connect both. Share VCC/GND/SDA/SCL; keep the first AD0 at GND and set the second AD0 to 3V3. Expect both **0x68 and 0x69** and separate readings. Moving one board should affect its readings independently.

Commands: **r** rescans and configures both devices; **s** scans only. A device stops being read after three consecutive read failures; fix wiring and send r. If no device responds, the failure occurs before chip identification. A scan cannot distinguish two devices colliding at one address. A missing device can cause driver-level NACK logs on some ESP32 core versions.

WHO_AM_I is separate from the I2C address: genuine MPU6050 chips return 0x68 at either address. Other accepted IDs are labelled alternative profiles. The 0x98 profile is experimental (`ALLOW_UNVERIFIED_0X98`); it is not evidence of genuine MPU6050 hardware or verified scale accuracy. Register configuration is read back before declaring READY. This is a raw sensor test, without wrist angles or medical interpretation.

If restarting, capture the text **before** `Rebooting...`, not just the ROM boot messages. Also report your Espressif Boards Manager version and the scan/identity output. Local build results are in `build-validation.txt`; compilation does not verify physical sensor operation.
