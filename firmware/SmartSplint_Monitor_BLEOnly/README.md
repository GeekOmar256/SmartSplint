# SmartSplint Monitor Bluetooth only

Separate BLE-only firmware for the ESP32-C3. Wi-Fi, HTTP, mDNS, credentials and saved transport selection have been removed. The original combined firmware is unchanged by this variant.

## Upload

- Board: ESP32C3 Dev Module.
- USB CDC On Boot: Enabled.
- Libraries: OneWire and DallasTemperature. Use the BLE library bundled with the ESP32 board package; no separate Bluetooth library is required.
- Locally compiled with esp32 core 2.0.17 and the default partition: 995,154 bytes (75% of 1,310,720). Huge APP is not required for this checked build. See build-validation.txt.
- Serial Monitor: 115200 baud. Commands: r reinitializes sensors; s scans I2C. Transport-switch commands w/b/a are not used.

## Sensors

| Connection | GPIO or destination |
|---|---|
| Both IMUs SDA / SCL | GPIO8 / GPIO9 |
| Forearm IMU AD0 | GND, address 0x68 |
| Hand IMU AD0 | 3V3, address 0x69 |
| FSR1 / FSR2 | GPIO0 / GPIO1, each with its own 10 kOhm divider resistor |
| DS18B20 DATA | GPIO3, 4.7 kOhm pull-up to 3V3 |

## Computer application

Enable Bluetooth on the computer and start the existing SmartSplint Monitor application using `python server.py` from its folder (not simulation mode). Select Bluetooth in the application's connection settings if another transport was previously saved. Device name: **SmartSplint**. The service UUID, notification characteristic UUID, 6-byte header and 37-byte sample format are unchanged from SmartSplint_Monitor_FW. No changes to the computer application's BLE decoder are required by this variant.

The ESP32 sends raw IMU/FSR readings on a nominal 50 Hz acquisition schedule and temperature about once per second. The computer app performs calibration and analysis and hosts the dashboard at **http://localhost:8000**. This firmware has no ESP32-hosted web page or IP address. A Bluetooth connection and adequate negotiated ATT MTU are required for sample notifications, as in the combined firmware.

Validation preserves the original acquisition and notification-encoder functions. Physical Bluetooth connection, reconnection and all-sensor operation still need testing on your board.

## Version 1.1 IMU reference alignment

The IMU register-write sequence now matches the supplied working AllSensors Wi-Fi reference, including register 0x19 = 19 for a 50 Hz internal rate. A 200 ms settling delay after I2C setup is also restored. Signed raw axis decoding and scale factors remain unchanged. Acquisition remains nominally 50 Hz for the computer app; the older reference only displayed/read IMUs every 500 ms.

Serial Monitor now shows both IMUs every 500 ms in g and degrees/s, or NO VALID SAMPLE with identity and error count. Every 5 seconds it also reports Bluetooth connection state, negotiated MTU and last sent sequence. If Serial axes move but the app has no data, investigate the Bluetooth link; if NO VALID SAMPLE appears, capture startup scan/identity/read-back output. An ATT MTU below 46 cannot carry the existing 6-byte header plus one 37-byte sample (and 3-byte ATT overhead).

The supplied reference displays sensor axes and has no wrist-angle algorithm. This firmware keeps raw packets compatible with the app; it does not duplicate scaling or filtering on the ESP32. Changing the sampling divider is not a confirmed diagnosis of the reported hardware fault. Physical testing remains necessary.
