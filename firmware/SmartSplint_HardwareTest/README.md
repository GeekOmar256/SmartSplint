# Smart Splint hardware test

Open `SmartSplint_HardwareTest.ino` in Arduino IDE. This is a USB/Serial and Wi-Fi dashboard bench test for the parts pictured in `Orthopedic components.docx`: ESP32-C3 SuperMini, two GY-521 MPU6050 modules, two FSR 400 sensors, and one DS18B20 waterproof probe. The requested two FSRs take precedence over the older three-FSR report design.

## Live Wi-Fi dashboard

The sketch now merges the sensor test with the existing Wi-Fi trial. It retains SSID `project`, password `12345678`, and hostname `smartsplint`. Edit the constants near the top of the sketch if needed. Use a 2.4 GHz network.

After upload, connect your phone/computer to the same network and open **http://smartsplint.local**, or use the IP printed in Serial Monitor. Some networks block device-to-device traffic; the IP address also works when mDNS is unavailable.

The page updates once per second after each completed request and shows:
- Two FSR raw counts, calibrated ADC millivolts, and saturation indicators.
- DS18B20 temperature in Celsius, or a missing/invalid status.
- Both MPU6050 acceleration axes (g), gyro axes (degrees/s), and read-error counts.
- Wi-Fi signal, IP address, uptime, and free heap.

`GET /api/status` returns the same data as JSON. Missing or invalid measurements are `null`, rather than stale numeric values. If the browser loses access to the board, it clears the displayed sensor readings. Wi-Fi reconnect attempts run every 10 seconds without waiting for a connection. Serial output continues independently. Sensor initialization/rescanning can briefly pause sampling and HTTP handling.

## Wiring

Disconnect power while wiring. Use the ESP32's **3V3** and **GND** for all sensors and a common ground. Start with USB power; leave the battery/boost circuit disconnected for this test. GPIO numbers below are the board labels, not physical header positions.

| Connection | ESP32-C3 / destination |
|---|---|
| Both MPU VCC | 3V3 |
| Both MPU GND | GND |
| Both MPU SDA | GPIO8 |
| Both MPU SCL | GPIO9 |
| Forearm MPU AD0 | GND (address 0x68) |
| Hand MPU AD0 | 3V3 (address 0x69) |
| Both MPU INT, XDA, XCL | Leave unconnected |
| FSR1 divider junction | GPIO0 |
| FSR2 divider junction | GPIO1 |
| DS18B20 VDD | 3V3 |
| DS18B20 GND | GND |
| DS18B20 DQ/data | GPIO10, plus **4.7 kOhm to 3V3** |

Each FSR needs its own **10 kOhm** resistor:

```text
3V3 ---- FSR1 ----+---- GPIO0
                 |
               10 kOhm
                 |
                GND

3V3 ---- FSR2 ----+---- GPIO1
                 |
               10 kOhm
                 |
                GND

3V3 ---- 4.7 kOhm ----+---- GPIO10
                     |
                 DS18B20 DQ
DS18B20 VDD -> 3V3; GND -> GND (three-wire powered mode)
```

FSRs have no polarity. Verify the temperature probe's wire labels with its seller; colors vary. Do not use parasite power. Do not apply 5 V to GPIOs. GY-521 boards normally include I2C pull-ups; if yours do not, add a pull-up from SDA to 3V3 and SCL to 3V3 (typically 4.7 kOhm each). GPIO8 also drives the SuperMini's onboard LED, so this sketch does not control that LED. GPIO8/9 are boot strapping pins: ensure the I2C bus is not held low during reset.

## Arduino IDE

1. Install **esp32 by Espressif Systems** in Boards Manager.
2. Select **ESP32C3 Dev Module** and the board's USB port.
3. Enable **USB CDC On Boot** for the SuperMini's native USB.
4. Install **OneWire by Paul Stoffregen** and **DallasTemperature by Miles Burton** from Library Manager. No MPU library is needed.
5. Upload, open Serial Monitor at **115200 baud**, then press RESET to see startup diagnostics. If upload will not connect, hold BOOT, tap RESET, release BOOT, and retry.

The sketch samples motion/FSRs on a nominal 20 ms schedule, prints every 500 ms, and updates temperature about once per second. I2C faults and diagnostics can extend the schedule; this is not a precision data logger. Send **r** to scan and initialize again after changing connections. Missing sensors do not halt the other tests. Successful IMU initialization is required before reads; reconnecting an IMU or temperature probe may require `r`.

## Checks

1. Startup scan should list **both 0x68 and 0x69**. Both MPU WHO_AM_I registers should read **0x68**, including the module at address 0x69. If only 0x68 appears, check the second module's AD0 connection; two devices at one address cannot be distinguished by the scan.
2. Place both modules flat and still: acceleration should be about 1 g in total magnitude, typically Z near +1 or -1 g and X/Y near zero. Gyro axes should be near zero, with some uncalibrated bias/noise.
3. Rotate each module separately. Only that module's acceleration direction and angular velocity should respond. This confirms two independent readings.
4. Release both FSRs: raw values should be low. Gently press each pad separately: its raw/mV value should increase. A disconnected FSR can also read low, so pressing is necessary to verify it. These values are **not calibrated force in newtons or pressure**.
5. A detected DS18B20 should produce a plausible room temperature after the first completed conversion; hold the probe and check that it rises gradually. A missing or invalid read is reported without displaying -127 C as a measurement. A persistent 85 C around room temperature suggests a conversion/power fault.

The ESP32-C3 ADC at 11 dB attenuation has a useful range of roughly 0â€“2.5 V, less than the 3.3 V divider supply. Strong pressure can therefore saturate the ADC even with correct wiring. `NEAR ADC LIMIT` flags raw >=4000; reduce pressure, or redesign the divider for calibrated measurements. Millivolts use the core's calibrated ADC API and are not calculated as raw/4095 * 3300.

This sketch tests the hardware's raw responses. It does not calculate wrist angles, calibrate gyro offsets, set medical thresholds, or store historical data. The original Wi-Fi-only trial is preserved separately. Mounting/alignment and a calibrated orientation filter are needed before evaluating wrist angle accuracy.

## Sources and build validation

- Component identities and existing pin assignments: project Word reports, component screenshots, and project notes.
- [MPU6050 register map](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf): register addresses and +/-2 g, +/-250 degrees/s scaling.
- [Espressif ADC API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html): raw and calibrated millivolt readings and C3 attenuation range.
- [DallasTemperature asynchronous example](https://github.com/milesburton/Arduino-Temperature-Control-Library/blob/master/examples/WaitForConversion/WaitForConversion.ino): nonblocking conversions.

Compile target: `esp32:esp32:esp32c3:CDCOnBoot=cdc`. The exact local build outcome is recorded in `build-validation.txt`. Physical sensor responses must be verified on the connected hardware.

The pre-merge Serial-only sketch is saved as `SmartSplint_HardwareTest.ino.before-wifi.bak`. The original Wi-Fi trial is unchanged. Dashboard logic checks: `node dashboard.test.cjs` (mocked HTTP and DOM, not a live ESP32 test).

## Diagnostic update (2026-10-17)

The supplied root-level `SmartSplint_HardwareTest_NoWiFi.ino` was reviewed and left unchanged. The Wi-Fi sketch now starts HTTP only after receiving a Wi-Fi connection/IP, restarts HTTP after reconnecting, and prints numeric disconnect and reset reasons. Its earlier version is saved as `SmartSplint_HardwareTest.ino.before-diagnostics.bak`.

IMU detection now recognizes the supplied NoWiFi sketch's alternative identities. 0x70/0x71/0x73 use the shared acceleration/gyro register layout with the newer accelerometer filter register. 0x98 is explicitly unverified and controlled by `ALLOW_UNVERIFIED_0X98`; it does not prove the sensor is a genuine MPU6050 or that readings are correct. Verify stationary gravity magnitude and independent axis response before relying on any alternative profile. WHO_AM_I is not the I2C bus address.

For a dual-IMU fault, disconnect power, test each board individually, then connect both with AD0 fixed LOW on one and HIGH on the other. The scan must show 0x68 and 0x69 together. If it does but configuration fails, capture both identity values and error messages. A scan alone cannot detect two boards colliding at the same address.

For Wi-Fi faults, capture the full startup output, disconnect reason, and any reset/panic message. ESP32-C3 needs a 2.4 GHz network. A Wi-Fi-triggered power reset is different from a password error or a web-server error; do not disable brownout detection to hide it. Start testing with a reliable USB supply/cable and short sensor wiring. Physical hardware operation is not established by compilation.
