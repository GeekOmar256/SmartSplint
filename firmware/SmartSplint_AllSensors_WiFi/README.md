# All sensors with Wi-Fi dashboard

New sketch based on `SmartSplint_AllSensors`, which you confirmed works. The dual-IMU initialization, 100 kHz I2C bus, sensor pins, FSR averaging, and asynchronous DS18B20 conversion are retained. Existing sketches are unchanged.

## Upload and open

1. Open `SmartSplint_AllSensors_WiFi.ino` in Arduino IDE.
2. Select **ESP32C3 Dev Module** and **USB CDC On Boot: Enabled**. Install **OneWire** and **DallasTemperature** if not already installed.
3. Network defaults from the Wi-Fi test are SSID **project**, password **12345678**. Edit `WIFI_SSID` and `WIFI_PASSWORD` near the top of the sketch if needed. Use a 2.4 GHz network.
4. Upload. Open Serial Monitor at **115200 baud**.
5. Connect your phone/computer to the same network. Open **http://smartsplint.local**, or the **http://IP-address** printed in Serial Monitor. Guest-network isolation can prevent access; some devices require the IP instead of mDNS.

## Working wiring

| Connection | GPIO / destination |
|---|---|
| Both IMUs SDA / SCL | GPIO8 / GPIO9 |
| First IMU AD0 | GND, address 0x68 |
| Second IMU AD0 | 3V3, address 0x69 |
| FSR1 / FSR2 divider junctions | GPIO0 / GPIO1 |
| DS18B20 DATA | **GPIO3**, plus 4.7 kOhm to 3V3 |
| Sensor power / ground | 3V3 / common GND |

Each FSR divider remains `3V3 -> FSR -> ADC junction -> 10 kOhm -> GND`. GPIO3 temperature wiring matches the confirmed all-sensors version; the early GPIO10 diagrams are not applicable. Use the successful wiring without changes. Do not drive the onboard LED sharing GPIO8.

## Display and diagnostics

The page displays both IMUs' acceleration (g) and gyro axes (degrees/s), both FSRs' raw counts and millivolts, temperature (Celsius), sensor status, IP, Wi-Fi signal, uptime, and free heap. Browser updates are about once per second. Serial frames and IMU reads remain every 500 ms; FSRs are sampled on a nominal 20 ms schedule and temperature about once per second. These are bench-test schedules, not precise acquisition guarantees.

The web server starts only after Wi-Fi obtains a connection and restarts after reconnection. Wi-Fi reconnect requests occur every 10 seconds without waiting in a connection loop. Sensor reads and Serial output continue if the network is unavailable. `/api/status` serves cached data and never performs I2C transactions. Failed measurements are represented as JSON `null`; failed browser requests clear the sensor display. No history is saved.

Serial commands: **r** rescans/reinitializes all sensors; **s** scans I2C only. Rescanning pauses normal processing briefly. After three consecutive read failures, an IMU stops until **r**, while the remaining sensors continue. FSR counts are not calibrated force and cannot detect disconnection by themselves. Wrist angles and medical thresholds are not included.

If Wi-Fi resets the board, preserve the message before `Rebooting...` and the Wi-Fi disconnect reason. Test with a reliable USB supply/cable; do not change working sensor wiring or disable brownout protection. `REDUCE_TX_POWER` remains an optional setting inherited from the Wi-Fi trial, not a confirmed fix.

Validation: compile target `esp32:esp32:esp32c3:CDCOnBoot=cdc`; build result in `build-validation.txt`. Run `node dashboard.test.cjs` for dashboard tests using mocked HTTP and DOM. Physical Wi-Fi and combined sensor operation must still be checked on your board.
