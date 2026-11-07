# SmartSplint Monitor

Computer application for the Smart Orthopedic Splint. The splint (ESP32-C3) sends **raw sensor data only**. This
application does all the processing, calibration, storage and analysis, and shows a dashboard in the web browser.

```
 Splint (ESP32-C3, firmware/SmartSplint_Monitor_FW)          Computer (this folder)
 ┌─────────────────────────────────────────┐     Wi-Fi     ┌────────────────────────────────────────┐
 │ 2 × MPU6050, 2 × FSR, DS18B20           │  or Bluetooth │ server.py                              │
 │ 50 samples/s, raw counts, 10 s buffer   │ ────────────► │  conversion, calibration, filter,     │
 │ no processing                           │               │  wrist angle, events, SQLite storage, │
 └─────────────────────────────────────────┘               │  analysis, alerts, CSV export          │
                                                           │ static/  →  dashboard in the browser   │
                                                           └────────────────────────────────────────┘
```

## Start

Requirements: **Python 3.8 or newer** and the Bluetooth package: `pip install bleak` (once). If you only use
Wi-Fi, the standard library is enough.

| How | What happens |
|---|---|
| Double-click **run.bat** | Connects to the splint and opens the dashboard at http://localhost:8000 |
| Double-click **run_simulation.bat** | Runs without hardware, using simulated raw data and 3 days of simulated history |
| `python server.py --host 192.168.1.50` | Uses a specific IP address for the splint (Wi-Fi) |
| `python server.py --port 8080` | Uses another dashboard port |

Simulation mode keeps its own database, settings and calibration (`data/*_simulation.*`), so simulated data never
mixes with real measurements, and a yellow banner is shown in the dashboard.

## Connecting the splint

1. Upload `firmware/SmartSplint_Monitor_FW` in the Arduino IDE. Use these settings:
   - Board: **ESP32C3 Dev Module**
   - USB CDC On Boot: **Enabled**
   - **Partition Scheme: Huge APP (3MB No OTA)**. Wi-Fi + Bluetooth do not fit in the default partition.
2. **Bluetooth is the default** on both the splint and the app, so they connect without any setup.
   To change the link on the splint, use the Serial Monitor (115200 baud). The choice is saved in the splint:
   - `b` = Bluetooth only (default, lowest power)
   - `w` = Wi-Fi only
   - `a` = both
3. In the app, the link is chosen in **Settings → Connection** (Bluetooth by default).

| | Wi-Fi | Bluetooth (BLE) |
|---|---|---|
| Needs | Same network as the computer (hotspot `project`, 2.4 GHz) | Computer with Bluetooth, within about 10 m |
| Power use | Higher | Much lower: better for battery use |
| Extra software | None | `pip install bleak` |
| Address | `smartsplint.local` or the IP shown on the Serial Monitor | Device name `SmartSplint` |

The splint keeps the last 10 s of samples. Short Wi-Fi interruptions therefore lose no data. Longer gaps are shown
in **Analysis → Data quality**.

## Calibration (saved on the computer, reused in later sessions)

Calibration is stored in `data/calibration.json` and loaded every time the app starts. Repeat it when the splint is
refitted or a sensor is replaced.

- **Motion sensors (IMU):**
  - Fit the splint and keep the arm still for 3 s.
  - The app measures the **gyroscope offsets** of both IMUs and the **fitted wrist angle** (the reference θ0).
  - If movement is detected, the calibration is rejected and has to be repeated.
- **FSR 1 and FSR 2:**
  - Record a **no-load zero**.
  - Place 2–4 known weights on the sensor and enter their mass in grams.
  - The app converts readings to **newtons** by linear interpolation between the points.
  - Until an FSR is calibrated, raw ADC readings are shown and analyzed instead.

## What the application calculates

| Item | Method |
|---|---|
| Segment angle | Complementary filter for each IMU: θ = 0.98·(θ + ω·Δt) + 0.02·θacc, with θacc = atan2(−ax, √(ay² + az²)) |
| Wrist movement | Δθwrist = (θhand − θforearm) − θ0. Whole-arm movement cancels out |
| Sideways movement | Integral of ω_rel = ωz,hand − ωz,forearm, reset after 1 s at rest |
| Movement event | Δθwrist above the angle limit (5°) or sideways movement above 8°. The event ends after 0.5 s below 60% of the limits |
| Pressure trend | Median of the baseline period vs the last 30 min. Alert if the rise is ≥ 20% for 30 min |
| Temperature trend | Same method. Alert if the rise is ≥ 1 °C |
| Compliance | % of time the wrist stayed within the angle limit of its fitted position |
| Loose splint | Movements per hour increasing day by day (or second half vs first half of the period) |
| Movement–pressure link | Pressure peaks that coincide with movement, plus their correlation |
| Data quality | Coverage, gaps, missed samples, IMU availability |

All limits can be changed in **Settings**. They were chosen by the project team and are **not clinically
validated**. The findings support the clinician and do not replace clinical judgement.

## Files

| File | Purpose |
|---|---|
| `server.py` | Connection (Wi-Fi / Bluetooth / simulation), processing, calibration, database, analysis, web server |
| `static/index.html`, `style.css`, `app.js` | Dashboard |
| `data/splint.db` | SQLite database: one row per second plus notes and markers |
| `data/settings.json`, `data/calibration.json` | Saved settings and calibration |

**Export:** use **Settings → Export CSV** for the selected period.

## Limitations

- Without a magnetometer, sideways (radial/ulnar) movement is measured as short movements, not as an absolute angle
  over long periods.
- FSR readings depend on how the load is applied. Calibrate with the sensor mounted as it is used.
- The power figures for Wi-Fi vs Bluetooth must be measured on the prototype.
