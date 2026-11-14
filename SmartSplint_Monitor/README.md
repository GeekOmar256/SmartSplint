# SmartSplint Monitor

Computer application for the Smart Orthopedic Splint. The splint (ESP32-C3) sends **raw sensor data only**. This
application does all the processing, calibration, storage and analysis, and shows a dashboard in the web browser.

The ESP32-C3 samples the two motion sensors and two force sensors at 50 Hz and updates the temperature about once per second. It sends the samples over BLE to `server.py`, which serves the dashboard locally.

## Start

Install the Python dependencies from the repository root with `python -m pip install -r requirements.txt`.

| How | What happens |
|---|---|
| Double-click **run.bat** | Connects to the splint and opens the dashboard at http://localhost:8000 |
| Double-click **run_simulation.bat** | Runs without hardware, with three days of simulated data for a new simulation database |
| `python server.py --port 8080` | Uses another dashboard port |

Simulation uses separate settings, calibration and storage. The dashboard identifies simulated readings with a banner.

## Connecting the splint

1. Upload `firmware/SmartSplint_Monitor_BLEOnly/SmartSplint_Monitor_BLEOnly.ino` to the **ESP32C3 Dev Module** with **USB CDC On Boot** enabled. Follow the firmware README for the board package, libraries and partition settings.
2. Enable Bluetooth on the computer and open **run.bat**. The app discovers the device named **SmartSplint**.
3. Open **Settings → Connection** to see the samples received and missed.

The dashboard opens on **Measurements**. The gear icon opens **Settings**, which contains session options, export and the professional calibration entry.

## Calibration (saved on the computer, reused in later sessions)

Open **Settings → Calibration** and enter the professional password. Calibration is stored in `data/calibration.json` and loaded every time the app starts. Repeat it when the splint is
refitted or a sensor is replaced.

- **Motion sensors (IMU):**
  - Fit the splint and keep the arm still for 3 s.
  - The app measures the **gyroscope offsets** of both IMUs and the **fitted wrist angle** (the reference θ0).
  - If movement is detected, the calibration is rejected and has to be repeated.
- **FSR 1 and FSR 2:**
  - Record a **no-load zero**.
  - Place 2–4 known weights on the sensor and enter their mass in grams.
  - The app converts readings to force by linear interpolation between the points, then to **pressure in kPa** by dividing by the FSR 400 active area (Ø 5.08 mm, 20.3 mm²).
  - Until an FSR is calibrated, its raw values appear on Measurements and are available for analysis.

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
| `server.py` | Connection (Bluetooth / simulation), processing, calibration, database, analysis, web server |
| `static/index.html`, `style.css`, `app.js` | Dashboard |
| `data/splint.db` | SQLite database: one row per second plus notes and markers |
| `data/settings.json`, `data/calibration.json` | Saved settings and calibration |

**Export:** use **Settings → Export CSV** for the selected period.

## Limitations

- Without a magnetometer, sideways (radial/ulnar) movement is measured as short movements, not as an absolute angle
  over long periods.
- FSR readings depend on how the load is applied. Calibrate with the sensor mounted as it is used.
- Battery runtime should be measured on the prototype.
