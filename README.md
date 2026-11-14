# Smart Orthopedic Splint

Final Year Project: a 3D-printed wrist splint with embedded sensors (2 × MPU6050, 2 × FSR, DS18B20) and an ESP32-C3
that sends raw data over Bluetooth Low Energy to a computer application for calibration,
storage, analysis and a web dashboard.

| Folder | Content |
|---|---|
| `SmartSplint_Monitor/` | Computer application (Python): `run.bat` for the real splint, `run_simulation.bat` without hardware |
| `firmware/SmartSplint_Monitor_BLEOnly/` | **Current firmware** for the ESP32-C3 (raw data over BLE) |
| `firmware/` (other folders) | Earlier test sketches (development history) |
| `docs/` | Code documentation, flowcharts and wiring diagram |

**Installing on a new computer:** follow `SETUP_COMMANDS.txt` step by step.

## Bluetooth only firmware

Use `firmware/SmartSplint_Monitor_BLEOnly/SmartSplint_Monitor_BLEOnly.ino` for a dedicated Bluetooth build without Wi-Fi or a device web server. It keeps the same sensor pins and BLE data format as the computer app. The checked build uses 75% of the default application partition with ESP32 core 2.0.17; Huge APP is not required for this variant. Enable USB CDC On Boot. See the firmware folder's README for connection and upload instructions.

Quick start, once Python is installed:

```
python -m pip install -r requirements.txt
cd SmartSplint_Monitor
python server.py --simulate --demo-days 3
```

Arduino IDE settings for the firmware:
- Board: **ESP32C3 Dev Module**, ESP32 core **2.0.17**
- USB CDC On Boot: **Enabled**
- Partition Scheme: **Default** (checked with ESP32 core 2.0.17)
- Libraries: **OneWire**, **DallasTemperature**

Note: the wiring images in `firmware/SmartSplint_HardwareTest/` are old and put the DS18B20 on GPIO10. The confirmed
wiring uses **GPIO3**; see `docs/flowcharts/wiring_diagram.png`.
