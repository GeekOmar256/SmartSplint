# Smart Orthopedic Splint: Code Documentation

This document describes the two programs of the monitoring system:

| Part | Location | Language | Runs on |
|---|---|---|---|
| **Firmware** | `firmware/SmartSplint_Monitor_BLEOnly/SmartSplint_Monitor_BLEOnly.ino` | C++ (Arduino) | ESP32-C3 SuperMini on the splint |
| **Computer application** (SmartSplint Monitor) | `SmartSplint_Monitor/` (`server.py` + `static/`) | Python 3 + HTML/CSS/JavaScript | The clinician's computer |

The main design rule is that **the splint only acquires and sends raw data, and the computer does all the processing**:
unit conversion, calibration, filtering, wrist angle, movement events, storage and analysis. This keeps the firmware
simple and lets the calibration and analysis change without reprogramming the splint.

Flowcharts (PNG and SVG) are in `docs/flowcharts/` and are drawn by `draw_flowcharts.py`
(run `python draw_flowcharts.py` after editing it). They are explained in
`Smart Orthopedic Splint - Software Flowcharts.docx`.

| Figure | File |
|---|---|
| ESP32 firmware: setup | `1a_esp32_setup.png` |
| ESP32 firmware: main loop | `1b_esp32_main_loop.png` |
| Application: startup and dashboard | `2a_app_startup_dashboard.png` |
| Application: data-link thread | `2b_app_data_link.png` |
| Processing step 1: conversion and filtering | `3a_processing_convert_filter.png` |
| Processing step 2: wrist movement, events and storage | `3b_processing_wrist_events_storage.png` |

---

## 1. System overview

```mermaid
%%{init: {"flowchart": {"curve": "step"}}}%%
flowchart LR
    subgraph SPLINT ["Splint (ESP32-C3)"]
        S1["2 × MPU6050 IMU<br/>(I2C 0x68, 0x69)"]
        S2["2 × FSR<br/>(ADC GPIO0, GPIO1)"]
        S3["DS18B20<br/>(1-Wire GPIO3)"]
        FW["Firmware: 50 Hz sampling<br/>ring buffer of 512 samples"]
        S1 --> FW
        S2 --> FW
        S3 --> FW
    end
    subgraph PC ["Computer"]
        APP["server.py: processing, calibration,<br/>SQLite storage, analysis, web server"]
        UI["Dashboard in the browser<br/>http://localhost:8000"]
        APP <--> UI
    end
    FW -- "Bluetooth LE<br/>binary notifications" --> APP
```

| Item | Value |
|---|---|
| Sampling rate | 50 samples/s (every 20 ms) for both IMUs and both FSRs, 1 sample/s for temperature |
| Raw sample | `seq, t_ms, forearm ax ay az gx gy gz, hand ax ay az gx gy gz, fsr1, fsr2, flags` |
| Buffer on the splint | 512 samples (about 10 s) |
| Data link | Bluetooth Low Energy |
| Stored on the computer | One row per second (averages, maxima, event count) in SQLite |

---

## 2. Firmware (ESP32-C3)

### 2.1 Board settings and libraries

| Setting | Value |
|---|---|
| Board | ESP32C3 Dev Module (ESP32 Arduino core 2.0.17) |
| USB CDC On Boot | Enabled (needed for the Serial Monitor) |
| **Partition Scheme** | Default, checked with ESP32 core 2.0.17 |
| Libraries | Wire and BLE (bundled); OneWire and DallasTemperature (installed) |
| Program size | 995,154 bytes (75% of the default partition) in the checked build |

### 2.2 Wiring

| Signal | ESP32-C3 pin | Notes |
|---|---|---|
| IMU SDA / SCL (both IMUs) | GPIO8 / GPIO9 | I2C at 100 kHz |
| Forearm IMU address | 0x68 | AD0 → GND |
| Hand IMU address | 0x69 | AD0 → 3.3 V |
| FSR1 (back of hand) | GPIO0 | Divider: 3.3 V → FSR → pin → 10 kΩ → GND |
| FSR2 (wrist) | GPIO1 | Same divider |
| DS18B20 data | GPIO3 | 4.7 kΩ pull-up to 3.3 V |

Do not use **GPIO2** for an FSR. It is a boot strapping pin, and the 10 kΩ pull-down can stop the board from
starting. GPIO8 also drives the on-board LED, so the LED is not used.

### 2.3 Timing and constants

| Constant | Value | Meaning |
|---|---|---|
| `SAMPLE_MS` | 20 | One sample every 20 ms (50 Hz) |
| `TEMP_MS` | 1000 | DS18B20 read once per second (12-bit conversion takes up to 750 ms) |
| `BUFFER_SIZE` | 512 | Ring buffer length (about 10 s) |
| IMU range | ±2 g, ±250 °/s | 16384 LSB/g and 131 LSB/(°/s) |
| IMU registers | `0x6B=0x01`, `0x6C=0x00`, `0x1A=0x03`, `0x19=19`, `0x1B=0x00`, `0x1C=0x00` | Wake, all axes on, 44 Hz anti-alias filter, 50 Hz internal rate, ranges |

### 2.4 Main functions

| Function | Purpose |
|---|---|
| `setup()` | Starts Serial, I2C and ADC, initializes the sensors and starts BLE advertising |
| `loop()` | Takes a sample every 20 ms, reads the temperature every second, services BLE and Serial commands, retries stopped IMUs every 5 s |
| `initIMU(i)` | Checks the IMU's identity (`WHO_AM_I`), writes the configuration and reads it back to confirm it |
| `readImuRaw(i)` | Reads 14 bytes from register `0x3B` and returns the 6 raw values (accel X/Y/Z, gyro X/Y/Z) |
| `takeSample()` | Reads both IMUs and both FSRs and stores one `Sample` in the ring buffer. After 10 failed reads in a row an IMU is marked not ready |
| `initializeTemperature()` | Finds the DS18B20, sets 12-bit resolution and starts non-blocking conversions |
| `startBLE()` | Creates the BLE server, the data service and its notify characteristic, and starts advertising |
| `serviceBLE()` | Packs up to 5 new samples into one binary packet and sends it as a notification |

### 2.5 Bluetooth Low Energy interface

| Item | Value |
|---|---|
| Device name | `SmartSplint` |
| Service UUID | `6e7a0001-5a3b-4c8e-9f21-7d4a2c1b0e01` |
| Data characteristic (notify) | `6e7a0002-5a3b-4c8e-9f21-7d4a2c1b0e01` |
| MTU | 247 requested. The number of samples per packet adapts to the negotiated MTU |
| Rate | One packet as soon as 5 samples are waiting, or every 100 ms (about 10 packets/s) |

In each sample, flags bit 0 marks a valid forearm IMU reading and bit 1 marks a valid hand IMU reading.

Packet layout (little-endian):

| Bytes | Field | Type |
|---|---|---|
| 0 | Version (= 1) | uint8 |
| 1 | Number of samples in the packet (1–5) | uint8 |
| 2 | Status: bit 0 forearm IMU ready, bit 1 hand IMU ready, bit 2 temperature valid | uint8 |
| 3–4 | Temperature × 100 (°C), `0x7FFF` = invalid | int16 |
| 5 | Reserved | uint8 |
| 6 + 37·n | Sample n: `seq` (uint32), `t_ms` (uint32), 6 × int16 forearm, 6 × int16 hand, `fsr1` (uint16), `fsr2` (uint16), `flags` (uint8) | 37 bytes |

### 2.6 Serial commands (115200 baud)

| Command | Action |
|---|---|
| `r` | Reinitialize all sensors |
| `s` | Scan the I2C bus (expects 0x68 and 0x69) |

### 2.7 Flowchart

![ESP32 firmware: setup](flowcharts/1a_esp32_setup.png)

![ESP32 firmware: main loop](flowcharts/1b_esp32_main_loop.png)

---

## 3. Computer application (SmartSplint Monitor)

### 3.1 Files and how to run

| File | Purpose |
|---|---|
| `server.py` | The whole back end: data link, processing, calibration, database, analysis and web server |
| `static/index.html`, `style.css`, `app.js` | Dashboard: Measurements, Settings and password-protected Calibration |
| `run.bat` | Starts the app for the real splint and opens the browser |
| `run_simulation.bat` | Starts the app with simulated raw data and 3 days of demo history |
| `data/splint.db` | SQLite database (one row per second + notes/markers) |
| `data/settings.json` | Saved settings (analysis limits, patient label, baseline) |
| `data/calibration.json` | **Saved IMU and FSR calibration, reused in every later session** |

Requirements: Python 3.8+ and `pip install bleak` (Bluetooth only). Options:

| Option | Meaning |
|---|---|
| `--simulate` | Run without hardware (separate `*_simulation` database, settings and calibration) |
| `--demo-days N` | With `--simulate`: create N days of history once, in an empty database |
| `--port N` | Dashboard port (default 8000) |
| `--no-browser` | Do not open the browser |

### 3.2 Program structure (`server.py`)

| Class / function | Purpose |
|---|---|
| `JsonStore`, `Settings` | Load and save `settings.json` and `calibration.json` (atomic write) |
| `Database` | SQLite access: `samples` table (one row per second) and `markers` table (calibration, baseline, notes) |
| `Processor` | Turns raw 50 Hz samples into angles, events and per-second rows. It also runs the calibration captures |
| `fsr_force()` / `fsr_pressure()` | Convert an FSR reading to force (N) with the saved calibration points, then to pressure (kPa) by dividing by the FSR 400 active area (Ø 5.08 mm = 20.3 mm²) |
| `parse_ble_packet()` | Decodes one BLE notification into raw sensor samples |
| `DeviceLink` (thread) | Gets data from the splint every 0.5 s by Bluetooth or simulation, and passes it to `Processor` |
| `Simulator` | Generates realistic raw samples (arm motion, wrist movements, gyro offsets, FSR loads, temperature) |
| `analyze()` | The analysis: pressure, temperature, movement, compliance, loose splint, movement–pressure link, data quality |
| `downsample()` | Reduces the selected period to at most 600 points for the history API |
| `Handler`, `App` | HTTP server: dashboard files and the JSON API |
| `create_demo_history()` | Simulated history and a demo calibration for testing the analysis |

### 3.3 Data link

| Link | How it works |
|---|---|
| **Bluetooth** | `bleak` finds the device named `SmartSplint` (8 s scan), connects and subscribes to the data characteristic. Packets are decoded and processed every 0.5 s. If no data arrives for 5 s, or the connection drops, it reconnects automatically |
| **Simulation** | `Simulator.batch()` produces the samples for the elapsed time |

Missed samples are detected from gaps in `seq`. A sequence number that goes backwards means the splint restarted,
and the filters are reset.

### 3.4 Processing of each raw sample

| Step | Calculation |
|---|---|
| Units | accel ÷ 16384 → g; gyro ÷ 131 → °/s; minus the saved gyro offsets |
| Tilt from gravity | θacc = atan2(−ax, √(ay² + az²)), with x along the segment and z normal to the back of the hand |
| Complementary filter (each IMU) | θ = α·(θ + ωy·Δt) + (1 − α)·θacc, α = 0.98, Δt from the splint's own timestamps. It restarts from θacc after a gap > 0.5 s |
| Wrist movement | Δθ = (θhand − θforearm) − θ0, where θ0 is the fitted angle saved at calibration. Whole-arm movement cancels out |
| Sideways movement | ωrel = ωz,hand − ωz,forearm, integrated over time. Reset to 0 after 1 s with \|ωrel\| < 3 °/s |
| Movement event | Starts when \|Δθ\| > 5° or \|sideways\| > 8°. Ends after 0.5 s with both below 60% of their limits |
| FSR pressure | Piecewise-linear interpolation between the calibration points (zero → 0 N), extrapolated above the last point, then divided by the sensor area to give kPa (1 N ≈ 49.3 kPa) |
| Storage | Each second: mean FSR readings and pressures, temperature, mean angles, maximum \|Δθ\| and \|ωrel\|, event count, IMU availability, samples received and missed |

![Processing step 1](flowcharts/3a_processing_convert_filter.png)

![Processing step 2](flowcharts/3b_processing_wrist_events_storage.png)

### 3.5 Calibration (saved in `calibration.json`)

| Calibration | Procedure | Check | Saved |
|---|---|---|---|
| **IMU** | Splint fitted, arm still, press Start (3 s capture) | ≥ 80% of samples with both IMUs OK; gyro standard deviation ≤ 2 °/s (otherwise "movement detected") | Gyro offsets of both IMUs (mean gyro), reference wrist angle θ0 (from the mean gravity direction) |
| **FSR zero** | No load, press Record zero (4 s) | Enough samples | Mean no-load reading |
| **FSR point** | Known weight on the sensor, enter grams, press Record weight (4 s) | Zero recorded first; reading at least 20 above zero | Point (reading, force in N). Older points that contradict it are removed |

Every capture ignores its first 0.3 s, because pressing the button can move the arm. Example file:

```json
{
  "imu": {"gyro_bias_dps": [[1.615, -0.893, 0.688], [-1.294, 1.102, -0.517]], "reference_deg": 24.995, "time": 1794036956},
  "fsr": [
    {"zero_raw": 2.6, "points": [{"raw": 1280.4, "force_n": 0.981}, {"raw": 2510.1, "force_n": 3.923}], "time": 1794037010},
    null
  ]
}
```

### 3.6 Analysis rules

Default limits can be changed in **Settings**. They were set by the project team and are not clinically validated.

| Analysis | Rule (default values) |
|---|---|
| Baseline | 10 min, starting at the time set with "Start new baseline", otherwise at the start of the selected period |
| Pressure | Median of the last 30 min vs the baseline median. **Alert** if the rise is ≥ 20% **and** ≥ 15 kPa (or ≥ 100 ADC if not calibrated). Uses kPa when ≥ 50% of the rows are calibrated |
| Temperature | Same method. **Alert** if the rise is ≥ 1.0 °C |
| Trends | Least-squares slope per hour (pressure and temperature) |
| Movement | Number of events, events per hour, largest movement. **Alert** if > 6 events per hour |
| Compliance | 100% − % of seconds with maximum \|Δθ\| > 5°. **Alert** if < 95% |
| Loose splint | With ≥ 3 days (≥ 2 h of data each): movements per hour, day by day. **Alert** if the last day is ≥ 1.5× the first and the trend is rising. With less data (≥ 6 h, ≥ 5 events): second half vs first half, ≥ 1.5× |
| Movement–pressure link | Pressure peak = reading > 1.15 × its mean over the previous 30 s (and > 0.1 N / 40 ADC). Counts movement seconds with a peak within ±2 s, plus the correlation. **Note** if ≥ 50% coincide (peaks are caused by movement, not swelling) |
| Data quality | Coverage (gaps > 10 s excluded), gaps, samples received/missed, % of time both IMUs worked. **Note** if coverage < 80% |

Pressure and temperature alerts need at least baseline + 30 min of data in the selected period.

### 3.7 HTTP API (used by the dashboard)

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | Dashboard |
| GET | `/api/state` | Connection, live values and settings. Calibration data requires the session token (polled every 1 s) |
| GET | `/api/history?hours=H` | Downsampled readings for the last H hours (0 = all) |
| GET | `/api/analysis?hours=H` | Analysis results and alerts (polled every 10 s) |
| GET | `/api/export.csv?hours=H` | CSV export of the stored rows |
| POST | `/api/settings` | Save analysis limits and session settings |
| POST | `/api/calibration/unlock` | Verify the professional password and return a token for calibration requests |
| POST | `/api/calibration/start` | `{"kind": "imu" \| "fsr_zero" \| "fsr_point", "fsr": 1\|2, "force_n": N}` |
| GET | `/api/calibration/status` | Capture progress and result |
| POST | `/api/calibration/clear` | `{"part": "imu" \| "fsr1" \| "fsr2"}` |
| POST | `/api/calibration/remove_point` | Remove one FSR calibration point |
| POST | `/api/baseline` | Start a new baseline now |
| POST | `/api/marker` | Save a note |

### 3.8 Database (`samples` table, one row per second)

| Column | Meaning | Unit |
|---|---|---|
| `ts` | Time (Unix seconds) | s |
| `fsr1_raw`, `fsr2_raw` | Mean FSR reading | ADC (0–4095) |
| `fsr1_kpa`, `fsr2_kpa` | Mean contact pressure (if calibrated). Older databases are converted from `fsr1_n`/`fsr2_n` on start | kPa |
| `temp_c` | Skin temperature | °C |
| `theta_f`, `theta_h` | Mean filtered forearm and hand angles | ° |
| `wrist_delta` | Mean wrist movement from the fitted position | ° |
| `deviation` | Sideways movement at the end of the second | ° |
| `omega_rel` | Relative rotation rate | °/s |
| `win_max_delta`, `win_max_omega` | Maximum \|Δθ\| and \|ωrel\| in the second | °, °/s |
| `events_total` | Cumulative movement events in this session | count |
| `in_event` | Movement in progress | 0/1 |
| `imu_f_ok`, `imu_h_ok` | Fraction of samples with the IMU working | 0–1 |
| `rssi` | Legacy Wi-Fi signal column | dBm |
| `calibrated` | Wrist movement available (IMU calibrated) | 0/1 |
| `n_samples`, `n_missed` | Samples received and missed in the second | count |

### 3.9 Dashboard

| Tab | Content |
|---|---|
| Measurements | Default page with live sensor readings, connection, alerts (from the analysis) and notes. Shows raw readings for uncalibrated sensors |
| Calibration | Open from Settings and unlock with the professional password. Includes IMU and FSR calibration procedures |
| Settings (gear) | Professional calibration entry, patient label, baseline, CSV export and alert limits |

The separate Analysis page was removed (2026-11-13). The analysis still runs on the computer and supplies the alerts,
the status banner and the compliance card on Measurements.

![Application: startup and dashboard](flowcharts/2a_app_startup_dashboard.png)

![Application: data-link thread](flowcharts/2b_app_data_link.png)

---

## 4. Testing done

| Test | Result |
|---|---|
| BLE-only firmware compile (`esp32:esp32:esp32c3`, default partition) | Success, 75% flash in the checked build |
| BLE packet encode → decode | The 191-byte, 5-sample packet decoded exactly |
| FSR conversion | Correct interpolation and extrapolation |
| Whole-arm motion (simulation) | Arm moving ±30°, wrist movement stayed ≈ 0° |
| IMU calibration (simulation) | Recovered the simulated gyro offsets (1.615 vs 1.6 °/s) and the 25.0° reference |
| Analysis (3 days of simulated data) | Rising movement rate detected as a loose splint. Analysis time 0.45 s |
| Dashboard | Default page, calibration access and uncalibrated readings checked in browser simulation |

**Not yet tested:** the new firmware on the real splint, and the Bluetooth link with real hardware.

## 5. Earlier sketches (development history)

| Sketch | Purpose |
|---|---|
| `SmartSplint_WiFiTest_1` | First Wi-Fi connection test with a simple web page |
| `SmartSplint_HardwareTest` | Sensor and wiring diagnostics |
| `SmartSplint_IMUOnly` | Reading both MPU6050s (0x68 and 0x69) |
| `SmartSplint_AllSensors` | All sensors, Serial output only |
| `SmartSplint_AllSensors_WiFi` | All sensors with an on-board web dashboard (values processed on the ESP32) |
| `SmartSplint_Monitor_FW` | Earlier dual-transport build |
| **`SmartSplint_Monitor_BLEOnly`** | **Current raw-data BLE firmware for the computer application** |

## 6. Known limitations

- The MPU6050 has no magnetometer. Sideways (radial/ulnar) movement is therefore measured as short movements, not as
  an absolute angle over long periods.
- The application reports gaps in sample sequence numbers across Bluetooth interruptions.
- The sign of the wrist angle depends on how the IMU boxes are mounted. Confirm it with a goniometer (test T3).
- The analysis limits are not clinically validated, and battery runtime still has to be measured.
