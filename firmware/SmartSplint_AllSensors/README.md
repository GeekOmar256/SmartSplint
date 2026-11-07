# All sensors, no Wi-Fi

This new sketch combines the IMU-only version that produced readings at both 0x68 and 0x69 with the FSR and DS18B20 routines from your latest pasted code. The earlier sketches are unchanged. Temperature DATA stays on **GPIO3**, as in your working pasted code; older diagrams showing GPIO10 do not match this sketch.

| Connection | ESP32-C3 SuperMini |
|---|---|
| Both IMU SDA | GPIO8 |
| Both IMU SCL | GPIO9 |
| IMU at 0x68 AD0 | GND |
| IMU at 0x69 AD0 | 3V3 |
| FSR1 divider junction | GPIO0 |
| FSR2 divider junction | GPIO1 |
| DS18B20 DATA | GPIO3, with 4.7 kOhm to 3V3 |
| Sensor power / common ground | 3V3 / GND |

Each FSR divider is `3V3 -> FSR -> ADC junction -> 10 kOhm -> GND`. Each FSR needs its own resistor. MPU INT, XDA and XCL stay unconnected. Keep the AD0 arrangement that successfully produced both IMU readings. Use USB power for this bench test and disconnect power before rewiring.

Select **ESP32C3 Dev Module**, enable **USB CDC On Boot**, install **OneWire** and **DallasTemperature**, and upload `SmartSplint_AllSensors.ino`. Open Serial Monitor at **115200 baud**. Send **r** to scan/reinitialize all sensors; **s** scans the I2C bus only.

Every 500 ms the output includes both IMUs' acceleration/gyro axes, both FSR raw counts and millivolts, and DS18B20 temperature or status. The FSRs are sampled on a nominal 20 ms schedule. IMU data is read for each output frame; its internal sample configuration is unchanged from the successful IMU test. Temperature conversion runs asynchronously about once per second. Rescanning can briefly pause acquisition.

Move each IMU separately, press each FSR separately, and warm the temperature probe by holding it. An unloaded or disconnected FSR may read low; raw counts are not calibrated force. Three consecutive read failures stop that IMU until **r**, while the remaining sensors continue. The newer MPU identity profiles, including the observed 0x70, are retained. The 0x98 profile remains explicitly experimental.

Compilation results are in `build-validation.txt`. The combined hardware still needs testing on your board. This sketch does not include Wi-Fi, wrist-angle calculations, or medical thresholds.
