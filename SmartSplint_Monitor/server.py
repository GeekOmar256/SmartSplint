#!/usr/bin/env python3
"""SmartSplint Monitor - computer application for the Smart Orthopedic Splint.

The ESP32-C3 sends raw sensor data only (50 samples per second). This application does all the
processing: unit conversion, IMU and FSR calibration, the complementary filter, the wrist angle,
movement events, storage, analysis and the dashboard (http://localhost:8000).

Only the Python standard library is used (Python 3.8 or newer), so nothing has to be installed.

    python server.py                    connect to the splint over Bluetooth
    python server.py --simulate         run without hardware, using simulated raw data
    python server.py --simulate --demo-days 3   also create 3 days of simulated history
"""
import argparse
import asyncio
import csv
import importlib.util
import io
import hmac
import secrets
import json
import math
import random
import sqlite3
import struct
import threading
import time
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

APP_DIR = Path(__file__).resolve().parent
STATIC_DIR = APP_DIR / "static"
DATA_DIR = APP_DIR / "data"
G = 9.80665
# FSR 400 active area: 5.08 mm diameter circle. Pressure = force / area, reported in kPa (1 kPa = 1000 Pa).
FSR_AREA_M2 = math.pi * (5.08e-3 / 2) ** 2

DEFAULT_SETTINGS = {
    "transport": "ble",             # "ble" (default) or "wifi"
    "ble_name": "SmartSplint",
    "device_host": "smartsplint.local",
    "poll_interval_s": 0.5,
    "patient_label": "",
    "baseline_ts": None,
    "baseline_minutes": 10,
    "pressure_rise_pct": 20.0,      # sustained pressure rise that raises a flag
    "pressure_rise_min_kpa": 15.0,  # ...and at least this much pressure in kPa (calibrated FSR)
    "pressure_rise_min_raw": 100,   # ...or this many ADC counts (uncalibrated FSR)
    "temp_rise_c": 1.0,
    "sustain_minutes": 30,
    "movement_angle_deg": 5.0,      # wrist movement inside the splint that counts as an event
    "deviation_angle_deg": 8.0,     # sideways movement that counts as an event
    "events_per_hour_limit": 6.0,
    "compliance_min_pct": 95.0,
    "loosening_ratio": 1.5,
    "filter_alpha": 0.98,
}

FIELDS = ["ts", "fsr1_raw", "fsr2_raw", "fsr1_kpa", "fsr2_kpa", "temp_c", "theta_f", "theta_h", "wrist_delta",
          "deviation", "omega_rel", "win_max_delta", "win_max_omega", "events_total", "in_event",
          "imu_f_ok", "imu_h_ok", "rssi", "calibrated", "n_samples", "n_missed"]


def clean(obj):
    """Replace NaN/inf with None so the JSON is valid."""
    if isinstance(obj, float):
        return obj if math.isfinite(obj) else None
    if isinstance(obj, dict):
        return {k: clean(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [clean(v) for v in obj]
    return obj


# ====================================================================== settings and calibration files
class JsonStore:
    def __init__(self, path, defaults):
        self.path = path
        self.lock = threading.Lock()
        self.values = json.loads(json.dumps(defaults))
        if path.exists():
            try:
                self.values.update(json.loads(path.read_text(encoding="utf-8")))
            except (OSError, ValueError):
                pass

    def get(self, key, default=None):
        with self.lock:
            return self.values.get(key, default)

    def all(self):
        with self.lock:
            return json.loads(json.dumps(self.values))

    def set(self, key, value):
        with self.lock:
            self.values[key] = value
            self._save()

    def _save(self):
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.values, indent=2), encoding="utf-8")
        tmp.replace(self.path)


class Settings(JsonStore):
    def update(self, changes):
        with self.lock:
            for key, value in changes.items():
                if key not in DEFAULT_SETTINGS:
                    continue
                default = DEFAULT_SETTINGS[key]
                if value is None:
                    self.values[key] = None
                elif isinstance(default, float):
                    self.values[key] = float(value)
                elif isinstance(default, int) and not isinstance(default, bool):
                    self.values[key] = int(float(value))
                elif isinstance(default, str):
                    self.values[key] = str(value).strip()
                else:
                    self.values[key] = value
            self._save()
            return json.loads(json.dumps(self.values))


EMPTY_CALIBRATION = {
    "imu": None,        # {"gyro_bias_dps": [[x,y,z],[x,y,z]], "reference_deg": .., "time": ..}
    "fsr": [None, None],  # per FSR: {"zero_raw": .., "points": [{"raw": .., "force_n": ..}], "time": ..}
}


def fsr_force(raw, cal):
    """Convert an FSR ADC reading to force (N) using the saved calibration points (piecewise linear)."""
    if raw is None or not cal or cal.get("zero_raw") is None or not cal.get("points"):
        return None
    zero = cal["zero_raw"]
    pts = [(zero, 0.0)] + sorted((p["raw"], p["force_n"]) for p in cal["points"] if p["raw"] > zero)
    if len(pts) < 2:
        return None
    if raw <= zero:
        return 0.0
    for (r0, f0), (r1, f1) in zip(pts, pts[1:]):
        if raw <= r1:
            return f0 + (f1 - f0) * (raw - r0) / (r1 - r0)
    (r0, f0), (r1, f1) = pts[-2], pts[-1]
    return max(0.0, f1 + (f1 - f0) * (raw - r1) / (r1 - r0))


def fsr_pressure(raw, cal):
    """Convert an FSR ADC reading to contact pressure in kPa: calibrated force divided by the sensor area."""
    f = fsr_force(raw, cal)
    return None if f is None else f / FSR_AREA_M2 / 1000.0


# ====================================================================== database
class Database:
    def __init__(self, path):
        self.lock = threading.Lock()
        self.conn = sqlite3.connect(str(path), check_same_thread=False)
        columns = ", ".join(f"{f} REAL" for f in FIELDS[1:])
        with self.lock:
            self.conn.execute(f"CREATE TABLE IF NOT EXISTS samples (ts REAL PRIMARY KEY, {columns})")
            self.conn.execute("CREATE TABLE IF NOT EXISTS markers (ts REAL, kind TEXT, text TEXT)")
            # older databases stored force in N (fsr1_n, fsr2_n): add the kPa columns and convert once
            have = {r[1] for r in self.conn.execute("PRAGMA table_info(samples)")}
            for k in (1, 2):
                if f"fsr{k}_kpa" not in have:
                    self.conn.execute(f"ALTER TABLE samples ADD COLUMN fsr{k}_kpa REAL")
                    if f"fsr{k}_n" in have:
                        self.conn.execute(f"UPDATE samples SET fsr{k}_kpa = fsr{k}_n / ? / 1000.0",
                                          (FSR_AREA_M2,))
            self.conn.commit()

    def insert_many(self, rows):
        if not rows:
            return
        with self.lock:
            self.conn.executemany(f"INSERT OR REPLACE INTO samples ({', '.join(FIELDS)}) "
                                  f"VALUES ({', '.join('?' * len(FIELDS))})",
                                  [[r.get(f) for f in FIELDS] for r in rows])
            self.conn.commit()

    def rows(self, start, end):
        with self.lock:
            cur = self.conn.execute(f"SELECT {', '.join(FIELDS)} FROM samples WHERE ts >= ? AND ts <= ? "
                                    "ORDER BY ts", (start, end))
            return [dict(zip(FIELDS, r)) for r in cur.fetchall()]

    def count(self):
        with self.lock:
            return self.conn.execute("SELECT COUNT(*), MIN(ts), MAX(ts) FROM samples").fetchone()

    def add_marker(self, kind, text, ts=None):
        with self.lock:
            self.conn.execute("INSERT INTO markers VALUES (?, ?, ?)", (ts or time.time(), kind, text))
            self.conn.commit()

    def markers(self, start, end):
        with self.lock:
            cur = self.conn.execute("SELECT ts, kind, text FROM markers WHERE ts >= ? AND ts <= ? ORDER BY ts",
                                    (start, end))
            return [{"ts": r[0], "kind": r[1], "text": r[2]} for r in cur.fetchall()]


# ====================================================================== signal processing (all done here)
def accel_pitch(ax, ay, az):
    """Pitch of a segment from gravity: x along the segment, z normal to the dorsal surface."""
    return math.degrees(math.atan2(-ax, math.sqrt(ay * ay + az * az)))


class Processor:
    """Turns raw 50 Hz samples into wrist angles, events and one stored row per second."""

    def __init__(self, calibration, settings):
        self.cal, self.settings = calibration, settings
        self.lock = threading.Lock()
        self.pitch = [None, None]
        self.last_t_ms = None
        self.last_seq = None
        self.deviation = 0.0
        self.rest_ms = 0.0
        self.in_event = False
        self.event_below_ms = 0.0
        self.events_total = 0
        self.bucket = None
        self.live = {}
        self.capture = None   # running calibration capture
        self.capture_result = None

    # ---------------------------------------------------------------- calibration captures
    def start_capture(self, kind, seconds, fsr_index=None, force_n=None):
        with self.lock:
            if self.capture:
                raise RuntimeError("Another calibration is already running")
            self.capture = {"kind": kind, "seconds": seconds, "fsr_index": fsr_index, "force_n": force_n,
                            "start": time.time(), "settle": 0.3, "n": 0, "n_imu": 0,
                            "g": [[0.0] * 3 for _ in range(2)], "g2": [[0.0] * 3 for _ in range(2)],
                            "a": [[0.0] * 3 for _ in range(2)], "fsr_sum": [0.0, 0.0], "fsr_sum2": [0.0, 0.0]}
            self.capture_result = None

    def capture_status(self):
        with self.lock:
            if self.capture:
                c = self.capture
                total = c["seconds"] + c["settle"]
                return {"running": True, "kind": c["kind"], "fsr": c["fsr_index"],
                        "progress": min(1.0, (time.time() - c["start"]) / total)}
            return {"running": False, "result": self.capture_result}

    def _capture_sample(self, f_ok, h_ok, acc, gyr, fsr):
        c = self.capture
        elapsed = time.time() - c["start"]
        if elapsed >= c["settle"] + c["seconds"]:
            self.capture = None
            try:
                self.capture_result = self._finish_capture(c)
            except Exception as exc:   # never let a calibration error stop the data link
                self.capture_result = {"ok": False, "kind": c["kind"], "message": f"Calibration error: {exc}"}
            return
        if elapsed < c["settle"]:      # ignore the movement of pressing the button
            return
        c["n"] += 1
        for k in range(2):
            c["fsr_sum"][k] += fsr[k]
            c["fsr_sum2"][k] += fsr[k] * fsr[k]
        if f_ok and h_ok:
            c["n_imu"] += 1
            for i in range(2):
                for k in range(3):
                    c["g"][i][k] += gyr[i][k]
                    c["g2"][i][k] += gyr[i][k] ** 2
                    c["a"][i][k] += acc[i][k]

    def _finish_capture(self, c):
        now = time.time()
        if c["n"] < 20:
            return {"ok": False, "kind": c["kind"], "message": "Not enough data received. Is the splint connected?"}
        if c["kind"] == "imu":
            n = c["n_imu"]
            if n < 0.8 * c["n"]:
                return {"ok": False, "kind": "imu", "message": "Both motion sensors must be working. Check the IMU wiring."}
            bias = [[c["g"][i][k] / n for k in range(3)] for i in range(2)]
            std = [[math.sqrt(max(0.0, c["g2"][i][k] / n - bias[i][k] ** 2)) for k in range(3)] for i in range(2)]
            if max(max(s) for s in std) > 2.0:
                return {"ok": False, "kind": "imu",
                        "message": "Movement was detected. Keep the arm and wrist completely still and try again."}
            acc = [[c["a"][i][k] / n for k in range(3)] for i in range(2)]
            pitch = [accel_pitch(*acc[i]) for i in range(2)]
            reference = pitch[1] - pitch[0]
            self.cal.set("imu", {"gyro_bias_dps": [[round(v, 4) for v in b] for b in bias],
                                 "reference_deg": round(reference, 3),
                                 "pitch_at_calibration": [round(p, 2) for p in pitch], "time": now})
            self.pitch = [None, None]     # restart the filters from gravity
            self.deviation = 0.0
            self.in_event = False
            return {"ok": True, "kind": "imu", "reference_deg": reference,
                    "message": f"Motion sensors calibrated. Fitted wrist angle: {reference:.1f}°"}
        k = c["fsr_index"]
        mean = c["fsr_sum"][k] / c["n"]
        std = math.sqrt(max(0.0, c["fsr_sum2"][k] / c["n"] - mean ** 2))
        cal_list = self.cal.get("fsr") or [None, None]
        entry = cal_list[k] or {"zero_raw": None, "points": [], "time": None}
        if c["kind"] == "fsr_zero":
            entry = {"zero_raw": round(mean, 1), "points": [p for p in entry.get("points", []) if p["raw"] > mean + 20],
                     "time": now}
            msg = f"FSR {k + 1} zero saved (no load = {mean:.0f})."
        else:
            if entry.get("zero_raw") is None:
                return {"ok": False, "kind": c["kind"], "message": "Record the no-load zero first."}
            if mean <= entry["zero_raw"] + 20:
                return {"ok": False, "kind": c["kind"],
                        "message": "The reading did not increase. Make sure the weight is resting on the sensor."}
            new = {"raw": round(mean, 1), "force_n": round(c["force_n"], 4)}
            # keep only old points that agree with the new one (more force must give a higher reading)
            points = [p for p in entry.get("points", [])
                      if abs(p["force_n"] - new["force_n"]) > 1e-6
                      and (p["raw"] - new["raw"]) * (p["force_n"] - new["force_n"]) > 0]
            points.append(new)
            entry["points"] = sorted(points, key=lambda p: p["raw"])
            entry["time"] = now
            kpa = c["force_n"] / FSR_AREA_M2 / 1000.0
            msg = f"FSR {k + 1}: {c['force_n'] / G * 1000:.0f} g ({kpa:.1f} kPa) point saved (reading {mean:.0f})."
        if std > 60:
            msg += " The reading was unsteady; consider repeating it."
        cal_list[k] = entry
        self.cal.set("fsr", cal_list)
        return {"ok": True, "kind": c["kind"], "fsr": k, "message": msg}

    # ---------------------------------------------------------------- per-sample processing
    def process(self, header, rows, wall_now):
        """Process one batch of raw rows. Returns finished one-second rows for the database."""
        finished = []
        with self.lock:
            imu_cal = self.cal.get("imu")
            fsr_cal = self.cal.get("fsr") or [None, None]
            s = self.settings.all()
            alpha = float(s["filter_alpha"])
            a_lsb = float(header.get("accel_lsb_per_g", 16384))
            g_lsb = float(header.get("gyro_lsb_per_dps", 131))
            uptime = header.get("uptime_ms")
            bias = imu_cal["gyro_bias_dps"] if imu_cal else [[0.0] * 3, [0.0] * 3]
            reference = imu_cal["reference_deg"] if imu_cal else None
            angle_thr = float(s["movement_angle_deg"])
            dev_thr = float(s["deviation_angle_deg"])
            for r in rows:
                seq, t_ms = r[0], r[1]
                flags = r[16]
                f_ok, h_ok = bool(flags & 1), bool(flags & 2)
                missed = 0
                if self.last_seq is not None:
                    if seq <= self.last_seq:          # device restarted
                        self.pitch = [None, None]
                        self.last_t_ms = None
                    elif seq > self.last_seq + 1:
                        missed = seq - self.last_seq - 1
                self.last_seq = seq
                acc = [[r[2 + 6 * i + k] / a_lsb for k in range(3)] for i in range(2)]
                gyr_raw = [[r[5 + 6 * i + k] / g_lsb for k in range(3)] for i in range(2)]
                gyr = [[gyr_raw[i][k] - bias[i][k] for k in range(3)] for i in range(2)]
                fsr = [r[14], r[15]]
                if self.capture:
                    self._capture_sample(f_ok, h_ok, acc, gyr_raw, fsr)

                dt = 0.02 if self.last_t_ms is None else (t_ms - self.last_t_ms) / 1000.0
                if dt <= 0 or dt > 0.5:   # gap: restart the filters from gravity
                    self.pitch = [None, None]
                    dt = 0.02
                self.last_t_ms = t_ms
                for i, ok in enumerate((f_ok, h_ok)):
                    if not ok:
                        self.pitch[i] = None
                        continue
                    p_acc = accel_pitch(*acc[i])
                    p = self.pitch[i]
                    self.pitch[i] = p_acc if p is None else alpha * (p + gyr[i][1] * dt) + (1 - alpha) * p_acc

                delta = omega = None
                if f_ok and h_ok and None not in self.pitch:
                    angle = self.pitch[1] - self.pitch[0]
                    delta = angle - reference if reference is not None else None
                    omega = gyr[1][2] - gyr[0][2]
                    if abs(omega) < 3.0:
                        self.rest_ms += dt * 1000
                        if self.rest_ms >= 1000:
                            self.deviation = 0.0
                    else:
                        self.rest_ms = 0.0
                        self.deviation += omega * dt
                    if delta is not None:
                        d, v = abs(delta), abs(self.deviation)
                        if not self.in_event and (d > angle_thr or v > dev_thr):
                            self.in_event = True
                            self.events_total += 1
                            self.event_below_ms = 0.0
                        elif self.in_event:
                            if d < 0.6 * angle_thr and v < 0.6 * dev_thr:
                                self.event_below_ms += dt * 1000
                                if self.event_below_ms >= 500:
                                    self.in_event = False
                            else:
                                self.event_below_ms = 0.0

                wall = wall_now - (uptime - t_ms) / 1000.0 if uptime is not None else wall_now
                second = math.floor(wall)
                if self.bucket and self.bucket["second"] != second:
                    finished.append(self._close_bucket(fsr_cal, header))
                    self.bucket = None
                if not self.bucket:
                    self.bucket = {"second": second, "n": 0, "missed": 0, "fsr": [0.0, 0.0], "f_ok": 0, "h_ok": 0,
                                   "pf": [], "ph": [], "delta": [], "max_delta": 0.0, "max_omega": 0.0,
                                   "has_delta": False}
                b = self.bucket
                b["n"] += 1
                b["missed"] += missed
                b["fsr"][0] += fsr[0]
                b["fsr"][1] += fsr[1]
                b["f_ok"] += f_ok
                b["h_ok"] += h_ok
                if self.pitch[0] is not None:
                    b["pf"].append(self.pitch[0])
                if self.pitch[1] is not None:
                    b["ph"].append(self.pitch[1])
                if delta is not None:
                    b["delta"].append(delta)
                    b["max_delta"] = max(b["max_delta"], abs(delta))
                    b["has_delta"] = True
                if omega is not None:
                    b["max_omega"] = max(b["max_omega"], abs(omega))
                b["deviation"] = self.deviation
                b["omega"] = omega
                # live values for the dashboard
                self.live = {"ts": wall, "fsr_raw": fsr,
                             "fsr_kpa": [fsr_pressure(fsr[k], fsr_cal[k]) for k in range(2)],
                             "theta_f": self.pitch[0], "theta_h": self.pitch[1], "wrist_delta": delta,
                             "deviation": self.deviation, "omega_rel": omega, "in_event": self.in_event,
                             "events_session": self.events_total, "imu_ok": [f_ok, h_ok],
                             "imu_raw": [r[2:8] if f_ok else None, r[8:14] if h_ok else None],
                             "temp_c": header.get("temp_c"), "calibrated": reference is not None}
        return finished

    def _close_bucket(self, fsr_cal, header):
        b = self.bucket
        n = max(1, b["n"])
        fsr_raw = [b["fsr"][k] / n for k in range(2)]
        mean = lambda v: sum(v) / len(v) if v else None
        return {"ts": float(b["second"]), "fsr1_raw": round(fsr_raw[0], 1), "fsr2_raw": round(fsr_raw[1], 1),
                "fsr1_kpa": fsr_pressure(fsr_raw[0], fsr_cal[0]), "fsr2_kpa": fsr_pressure(fsr_raw[1], fsr_cal[1]),
                "temp_c": header.get("temp_c"), "theta_f": mean(b["pf"]), "theta_h": mean(b["ph"]),
                "wrist_delta": mean(b["delta"]), "deviation": b.get("deviation"), "omega_rel": b.get("omega"),
                "win_max_delta": b["max_delta"] if b["has_delta"] else None,
                "win_max_omega": b["max_omega"], "events_total": self.events_total, "in_event": int(self.in_event),
                "imu_f_ok": b["f_ok"] / n, "imu_h_ok": b["h_ok"] / n, "rssi": header.get("rssi"),
                "calibrated": 1 if b["has_delta"] else 0, "n_samples": b["n"], "n_missed": b["missed"]}

    def snapshot(self):
        with self.lock:
            return dict(self.live)


# ====================================================================== simulator (raw data, same format as firmware)
def sim_fsr_raw(force_n):
    """FSR in a voltage divider with 10 kOhm: resistance falls as force rises."""
    if force_n <= 0.02:
        return 0.0
    r_fsr = min(1e6, 22000.0 / force_n ** 0.9)
    volts = 3.3 * 10000.0 / (10000.0 + r_fsr)
    return volts / 3.3 * 4095.0


class Simulator:
    BIAS = [[1.6, -0.9, 0.7], [-1.3, 1.1, -0.5]]   # deg/s gyro offsets, removed by calibration

    def __init__(self, seed=1, start=None):
        self.rng = random.Random(seed)
        self.seq = 0
        self.start_wall = start or time.time()
        self.last_wall = self.start_wall
        self.t_ms = 0
        self.prev = None
        self.event = None
        self.dev = 0.0
        self.force_override = [None, None]   # (force, until) while calibrating the FSRs in simulation
        self.still_until = 0.0
        self.last_arm = 0.0

    def truth(self, t, days):
        """True arm pitch, wrist movement, sideways movement and forces at time t (seconds)."""
        rng = self.rng
        still = t < self.still_until
        if still:
            arm = self.last_arm          # hold the arm where it is (simulates keeping still)
        else:
            arm = 18 * math.sin(2 * math.pi * t / 37) + 12 * math.sin(2 * math.pi * t / 211)
            if self.still_until and t - self.still_until < 3:   # move back smoothly after holding still
                w = (t - self.still_until) / 3
                arm = self.last_arm * (1 - w) + arm * w
            else:
                self.last_arm = arm
        rate_per_h = 4.0 + 3.0 * days
        if self.event is None and not still and rng.random() < rate_per_h / 3600 / 50:
            self.event = {"t0": t, "dur": rng.uniform(0.8, 2.5), "peak": rng.uniform(7, 16) * rng.choice([-1, 1]),
                          "side": rng.uniform(-12, 12)}
        wrist = side = 0.0
        moving = False
        if self.event:
            x = (t - self.event["t0"]) / self.event["dur"]
            if x >= 1:
                self.event = None
            else:
                bump = math.sin(math.pi * x)
                wrist, side, moving = self.event["peak"] * bump, self.event["side"] * bump, True
        hours = days * 24
        swell = 1 + 0.15 * math.sin(math.pi * min(hours, 48) / 48)
        spike = 1.3 if moving else 1.0
        forces = [2.0 * swell * spike, 1.6 * swell ** 2 * spike]
        temp = 33.6 + 0.4 * math.sin(2 * math.pi * hours / 24) + (0.9 if 30 < hours < 40 else 0)
        return arm, wrist, side, forces, temp, moving

    def batch(self, now_wall):
        rng = self.rng
        n = int((now_wall - self.last_wall) * 50)
        rows = []
        temp = 33.6
        for _ in range(min(n, 250)):
            self.t_ms += 20
            self.last_wall += 0.02
            t = self.t_ms / 1000.0
            days = (self.last_wall - self.start_wall) / 86400.0
            arm, wrist, side, forces, temp, _ = self.truth(t, days)
            pf, ph = 3 + arm, 3 + arm + 25 + wrist
            if self.prev is None:
                self.prev = (pf, ph, side)
            dpf, dph, dside = (pf - self.prev[0]) / 0.02, (ph - self.prev[1]) / 0.02, (side - self.prev[2]) / 0.02
            self.prev = (pf, ph, side)
            row = [self.seq + 1, self.t_ms]
            for i, (p, dp, dz) in enumerate(((pf, dpf, 0.0), (ph, dph, dside))):
                rad = math.radians(p)
                acc = [-math.sin(rad), 0.0, math.cos(rad)]
                gyr = [0.0, dp, dz]
                row += [round((acc[k] + rng.gauss(0, 0.008)) * 16384) for k in range(3)]
                row += [round((gyr[k] + self.BIAS[i][k] + rng.gauss(0, 0.15)) * 131) for k in range(3)]
            for k in range(2):
                ov = self.force_override[k]
                f = ov[0] if ov is not None and time.time() < ov[1] else forces[k]
                row.append(max(0, min(4095, round(sim_fsr_raw(f) + rng.gauss(0, 6)))))
            row.append(3)
            self.seq += 1
            rows.append(row)
        if n > 250:
            self.last_wall = now_wall
        header = {"firmware": "SIMULATOR", "hostname": "simulator", "ip": "127.0.0.1", "rssi": -52,
                  "uptime_ms": self.t_ms, "rate_hz": 50, "accel_lsb_per_g": 16384, "gyro_lsb_per_dps": 131,
                  "imu_ready": [True, True], "temp_found": True, "temp_c": round(temp + rng.gauss(0, 0.04), 2)}
        return header, rows


# ====================================================================== Bluetooth (BLE) packet format
BLE_DATA_UUID = "6e7a0002-5a3b-4c8e-9f21-7d4a2c1b0e01"
BLE_HEADER = struct.Struct("<BBBhB")        # version, count, status, temperature x100, reserved
BLE_SAMPLE = struct.Struct("<II6h6hHHB")    # same 17 fields as a Wi-Fi row


def ble_available():
    return importlib.util.find_spec("bleak") is not None


def parse_ble_packet(data):
    """Decode one BLE notification into (header, rows) in the same format as the Wi-Fi API."""
    version, count, status, temp, _ = BLE_HEADER.unpack_from(data, 0)
    if version != 1 or len(data) < BLE_HEADER.size + count * BLE_SAMPLE.size:
        raise ValueError("unexpected BLE packet")
    rows = [list(BLE_SAMPLE.unpack_from(data, BLE_HEADER.size + n * BLE_SAMPLE.size)) for n in range(count)]
    header = {"firmware": "SmartSplint (BLE)", "link": "ble", "accel_lsb_per_g": 16384, "gyro_lsb_per_dps": 131,
              "rate_hz": 50, "imu_ready": [bool(status & 1), bool(status & 2)], "temp_found": True,
              "temp_c": temp / 100.0 if status & 4 and temp != 0x7FFF else None,
              "uptime_ms": rows[-1][1] if rows else None, "rssi": None}
    return header, rows


# ====================================================================== connection to the splint
class DeviceLink(threading.Thread):
    def __init__(self, db, settings, processor, simulate=False):
        super().__init__(daemon=True)
        self.db, self.settings, self.processor, self.simulate = db, settings, processor, simulate
        self.sim = Simulator() if simulate else None
        self.lock = threading.Lock()
        self.state = {"connected": False, "last_ok": None, "error": "Not connected yet", "device": None,
                      "resolved_ip": None, "rows_received": 0, "rows_missed": 0}

    def _store(self, batches, started):
        stored = []
        for header, rows in batches:
            stored += self.processor.process(header, rows, time.time())
        self.db.insert_many(stored)
        header = batches[-1][0] if batches else None
        with self.lock:
            self.state.update(connected=True, last_ok=started, error=None)
            if header:
                self.state["device"] = header
            self.state["rows_received"] += sum(len(r) for _, r in batches)
            self.state["rows_missed"] += sum(int(r.get("n_missed") or 0) for r in stored)

    def _fail(self, message):
        with self.lock:
            self.state.update(connected=False, error=message)

    # ---------------------------------------------------------------- Bluetooth
    def _run_ble(self):
        if not ble_available():
            self._fail("Bluetooth needs the 'bleak' package. Close the app and run:  pip install bleak")
            time.sleep(3)
            return
        asyncio.run(self._ble_session())

    async def _ble_session(self):
        import bleak
        name = self.settings.get("ble_name") or "SmartSplint"
        self._fail(f"Searching for '{name}' over Bluetooth...")
        device = await bleak.BleakScanner.find_device_by_name(name, timeout=8.0)
        if device is None:
            raise RuntimeError(f"'{name}' not found over Bluetooth. Is the splint on and in BLE mode?")
        queue = []

        def on_data(_, data):
            try:
                queue.append(parse_ble_packet(bytes(data)))
            except (ValueError, struct.error):
                pass

        async with bleak.BleakClient(device) as client:
            await client.start_notify(BLE_DATA_UUID, on_data)
            self._fail("Bluetooth connected, waiting for data...")
            silent = 0.0
            while client.is_connected:
                await asyncio.sleep(0.5)
                if queue:
                    batch, queue[:] = list(queue), []
                    self._store(batch, time.time())
                    silent = 0.0
                else:
                    silent += 0.5
                    if silent >= 5:
                        raise RuntimeError("Bluetooth connected but no data is arriving")
            if not client.is_connected:
                raise RuntimeError("Bluetooth connection lost; reconnecting...")

    # ---------------------------------------------------------------- main loop
    def run(self):
        while True:
            if not self.simulate:
                try:
                    self._run_ble()
                except Exception as exc:
                    msg = exc.args[0] if exc.args and isinstance(exc.args[0], str) else str(exc)
                    if "powered on" in msg or "not available" in msg.lower():
                        msg = "Bluetooth is turned off on this computer. Turn it on in Windows Settings → Bluetooth."
                    self._fail(msg)
                    time.sleep(2)
                continue
            started = time.time()
            try:
                self._store([self.sim.batch(started)], started)
            except Exception as exc:              # anything else: show the real error
                self._fail(f"Processing error: {type(exc).__name__}: {exc}")
            interval = max(0.2, float(self.settings.get("poll_interval_s") or 0.5))
            time.sleep(max(0.05, interval - (time.time() - started)))

    def snapshot(self):
        with self.lock:
            return dict(self.state)


# ====================================================================== analysis
def median(values):
    v = sorted(x for x in values if x is not None)
    if not v:
        return None
    n = len(v)
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2


def slope(xs, ys, min_points=10):
    pts = [(x, y) for x, y in zip(xs, ys) if y is not None]
    if len(pts) < min_points:
        return None
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    den = sum((p[0] - mx) ** 2 for p in pts)
    return None if den == 0 else sum((p[0] - mx) * (p[1] - my) for p in pts) / den


def pearson(xs, ys):
    pts = [(x, y) for x, y in zip(xs, ys) if x is not None and y is not None]
    if len(pts) < 10:
        return None
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    sx = math.sqrt(sum((p[0] - mx) ** 2 for p in pts))
    sy = math.sqrt(sum((p[1] - my) ** 2 for p in pts))
    return None if sx == 0 or sy == 0 else sum((p[0] - mx) * (p[1] - my) for p in pts) / (sx * sy)


def count_events(rows):
    """Events from the cumulative counter; a drop means the application or device restarted."""
    total, prev, times = 0, None, []
    for r in rows:
        cur = r["events_total"]
        if cur is None:
            continue
        if prev is not None:
            inc = cur - prev if cur >= prev else cur
            if inc > 0:
                total += int(inc)
                times.extend([r["ts"]] * int(inc))
        prev = cur
    return total, times


def fsr_unit(rows):
    """Use kPa when most rows have calibrated pressure, otherwise raw ADC counts."""
    use = {}
    for k in (1, 2):
        n_cal = sum(1 for r in rows if r[f"fsr{k}_kpa"] is not None)
        use[k] = ("kpa", f"fsr{k}_kpa", "kPa") if rows and n_cal >= 0.5 * len(rows) else ("raw", f"fsr{k}_raw", "raw")
    return use


def analyze(rows, s, start, end, markers):
    out = {"window": {"start": start, "end": end, "hours": (end - start) / 3600.0}, "alerts": []}
    alerts = out["alerts"]
    if not rows:
        out["empty"] = True
        alerts.append({"level": "info", "title": "No data in this period",
                       "detail": "Connect the splint or choose a longer period."})
        return out
    ts = [r["ts"] for r in rows]
    span = ts[-1] - ts[0]
    received = sum(int(r["n_samples"] or 0) for r in rows)
    missed = sum(int(r["n_missed"] or 0) for r in rows)
    gap_list = [b - a for a, b in zip(ts, ts[1:]) if b - a > 10]
    gaps = len(gap_list)
    coverage = 100.0 * (1 - sum(gap_list) / span) if span > 0 else 100.0
    out["data_quality"] = {"seconds": len(rows), "coverage_pct": coverage, "gaps": gaps,
                           "samples": received, "missed": missed, "first": ts[0], "last": ts[-1],
                           "imu_ok_pct": 100 * sum(min(r["imu_f_ok"] or 0, r["imu_h_ok"] or 0) for r in rows) / len(rows)}

    bl_len = 60 * float(s["baseline_minutes"])
    b_ts = s.get("baseline_ts")
    if b_ts and ts[0] <= b_ts <= ts[-1]:
        b_start, b_source = b_ts, "set by you"
    else:
        b_start, b_source = ts[0], "start of the selected period"
    base = [r for r in rows if b_start <= r["ts"] <= b_start + bl_len]
    sustain = 60 * float(s["sustain_minutes"])
    recent = [r for r in rows if r["ts"] >= ts[-1] - sustain]
    out["baseline"] = {"start": b_start, "minutes": s["baseline_minutes"], "source": b_source}
    enough_time = span >= bl_len + sustain

    # ---- pressure
    units = fsr_unit(rows)
    pressure = []
    for k, name in ((1, "Back of hand (FSR 1)"), (2, "Wrist (FSR 2)")):
        kind, key, unit = units[k]
        b = median(r[key] for r in base)
        c = median(r[key] for r in recent)
        vals = [r[key] for r in rows if r[key] is not None]
        sl = slope(ts, [r[key] for r in rows])
        info = {"sensor": name, "unit": unit, "baseline": b, "recent": c, "max": max(vals) if vals else None,
                "slope_per_h": sl * 3600 if sl is not None else None, "flag": False}
        min_abs = float(s["pressure_rise_min_kpa"] if kind == "kpa" else s["pressure_rise_min_raw"])
        if b is not None and c is not None:
            info["change"] = c - b
            info["change_pct"] = (c - b) / b * 100 if b > 0 else None
            if info["change_pct"] is not None and info["change_pct"] >= s["pressure_rise_pct"] \
                    and info["change"] >= min_abs and enough_time:
                info["flag"] = True
                alerts.append({"level": "warning", "title": f"Sustained pressure rise: {name}",
                               "detail": f"Pressure stayed {info['change_pct']:.0f}% above the baseline for the last "
                                         f"{s['sustain_minutes']} min ({b:.2f} → {c:.2f} {unit}). This may indicate "
                                         "swelling. Please check the limb."})
        if kind == "raw":
            info["note"] = "Uncalibrated sensor readings"
        pressure.append(info)
    out["pressure"] = pressure

    # ---- temperature
    tb = median(r["temp_c"] for r in base)
    tc = median(r["temp_c"] for r in recent)
    temps = [r["temp_c"] for r in rows if r["temp_c"] is not None]
    sl = slope(ts, [r["temp_c"] for r in rows])
    temp = {"baseline": tb, "recent": tc, "min": min(temps) if temps else None, "max": max(temps) if temps else None,
            "slope_per_h": sl * 3600 if sl is not None else None, "flag": False}
    if tb is not None and tc is not None:
        temp["change"] = tc - tb
        if temp["change"] >= s["temp_rise_c"] and enough_time:
            temp["flag"] = True
            alerts.append({"level": "warning", "title": "Sustained skin temperature rise",
                           "detail": f"Skin temperature stayed {temp['change']:.1f} °C above the baseline for the last "
                                     f"{s['sustain_minutes']} min ({tb:.1f} → {tc:.1f} °C). This may indicate "
                                     "inflammation. Please check the limb."})
    out["temperature"] = temp

    # ---- wrist movement and compliance
    total_events, event_times = count_events(rows)
    hours = span / 3600.0
    thr = float(s["movement_angle_deg"])
    deltas = [r["win_max_delta"] for r in rows if r["win_max_delta"] is not None]
    outside = sum(1 for d in deltas if d > thr)
    compliance = 100.0 - 100.0 * outside / len(deltas) if deltas else None
    per_day = {}
    for t in event_times:
        d = time.strftime("%Y-%m-%d", time.localtime(t))
        per_day[d] = per_day.get(d, 0) + 1
    # recorded hours per day (gaps longer than 10 s are not counted), to compare days fairly
    hours_per_day = {}
    for a, b in zip(ts, ts[1:]):
        if b - a <= 10:
            d = time.strftime("%Y-%m-%d", time.localtime(a))
            hours_per_day[d] = hours_per_day.get(d, 0.0) + (b - a) / 3600.0
    rate_per_day = {d: per_day.get(d, 0) / h for d, h in hours_per_day.items() if h >= 2}
    movement = {"events": total_events, "events_per_hour": total_events / hours if span >= 600 else None,
                "rate_per_day": rate_per_day,
                "max_delta": max(deltas) if deltas else None,
                "time_outside_pct": 100.0 * outside / len(deltas) if deltas else None,
                "compliance_pct": compliance, "per_day": per_day, "calibrated": bool(deltas)}
    if not deltas:
        alerts.append({"level": "info", "title": "Motion sensors not calibrated",
                       "detail": "Open Calibration and calibrate the motion sensors while the splint is fitted."})
    if movement["events_per_hour"] is not None and movement["events_per_hour"] > s["events_per_hour_limit"]:
        alerts.append({"level": "warning", "title": "Frequent wrist movement inside the splint",
                       "detail": f"{movement['events_per_hour']:.1f} movements per hour (limit "
                                 f"{s['events_per_hour_limit']:.0f}). The wrist is not being kept still, or the splint "
                                 "may be loose."})
    if compliance is not None and compliance < s["compliance_min_pct"]:
        alerts.append({"level": "warning", "title": "Low immobilization compliance",
                       "detail": f"The wrist was more than {thr:.0f}° from its fitted position "
                                 f"{movement['time_outside_pct']:.1f}% of the time."})

    # ---- loose splint: movement rate rising over time
    loosening = {"flag": False, "method": None}
    ratio = float(s["loosening_ratio"])
    days = sorted(rate_per_day.items())
    if len(days) >= 3:
        rates = [round(r, 2) for _, r in days]
        loosening.update(method="movements per hour, day by day", first=rates[0], last=rates[-1],
                         trend_per_day=slope(list(range(len(rates))), rates, min_points=3))
        if rates[0] > 0 and rates[-1] / rates[0] >= ratio and loosening["trend_per_day"] > 0:
            loosening["flag"] = True
    elif span >= 6 * 3600 and total_events >= 5:
        mid = ts[0] + span / 2
        a = sum(1 for t in event_times if t < mid)
        loosening.update(method="first half vs second half", first=a, last=total_events - a)
        if a > 0 and (total_events - a) / a >= ratio:
            loosening["flag"] = True
    if loosening["flag"]:
        alerts.append({"level": "warning", "title": "Possible loose splint",
                       "detail": f"Wrist movement increased from {loosening['first']} to {loosening['last']} "
                                 f"({loosening['method']}). The splint may have loosened, for example after the "
                                 "swelling went down. Consider adjusting it."})
    movement["loosening"] = loosening
    out["movement"] = movement

    # ---- link between movement and pressure
    keys = [units[1][1], units[2][1]]
    hist = {k: [] for k in keys}
    spikes, devs, moves = [], [], []
    for r in rows:
        spike, total = False, 0.0
        for k in keys:
            v, h = r[k], hist[k]
            if v is not None and len(h) >= 5:
                ref = sum(h) / len(h)
                total += v - ref
                if ref > 0 and v > ref * 1.15 and v - ref > (5.0 if k.endswith("_kpa") else 40):
                    spike = True
            if v is not None:
                h.append(v)
                if len(h) > 30:
                    h.pop(0)
        spikes.append(spike)
        devs.append(total)
        moves.append(r["win_max_delta"])
    moving = coincident = 0
    for i, r in enumerate(rows):
        if r["win_max_delta"] is not None and r["win_max_delta"] > thr:
            moving += 1
            if any(spikes[max(0, i - 2):i + 3]):
                coincident += 1
    out["movement_pressure"] = {"moving_seconds": moving, "with_pressure_peak": coincident,
                                "pct": 100.0 * coincident / moving if moving else None,
                                "pressure_peaks": sum(spikes), "correlation": pearson(moves, devs)}
    if moving >= 5 and coincident / moving >= 0.5:
        alerts.append({"level": "info", "title": "Pressure peaks linked to movement",
                       "detail": f"{coincident} of {moving} seconds with wrist movement also had a pressure peak. "
                                 "These peaks come from the limb pushing against the splint, not from swelling."})

    if coverage < 80 and span > 600:
        alerts.append({"level": "info", "title": "Gaps in the recording",
                       "detail": f"Data was received for {coverage:.0f}% of the period ({gaps} gaps). The splint may "
                                 "have been out of Bluetooth range or switched off."})
    if not enough_time:
        alerts.append({"level": "info", "title": "Collecting data for trend analysis",
                       "detail": f"Pressure and temperature trends need at least "
                                 f"{s['baseline_minutes'] + s['sustain_minutes']} minutes of data."})
    if not any(a["level"] == "warning" for a in alerts):
        alerts.insert(0, {"level": "ok", "title": "Everything looks good",
                          "detail": "Pressure, temperature and wrist movement are within the set limits."})
    out["markers"] = markers
    out["fsr_units"] = {k: v[2] for k, v in units.items()}
    return out


def downsample(rows, start, end, max_points=600):
    if not rows:
        return {"t": []}
    units = fsr_unit(rows)
    bucket = max(1.0, (end - start) / max_points)
    groups = {}
    for r in rows:
        groups.setdefault(int((r["ts"] - start) // bucket), []).append(r)
    _, event_times = count_events(rows)
    ev = {}
    for t in event_times:
        b = int((t - start) // bucket)
        ev[b] = ev.get(b, 0) + 1
    out = {k: [] for k in ("t", "fsr1", "fsr2", "temp", "delta", "max_delta", "deviation", "events")}
    for b in sorted(groups):
        rs = groups[b]

        def avg(key):
            v = [r[key] for r in rs if r[key] is not None]
            return round(sum(v) / len(v), 3) if v else None

        def mx(key):
            v = [abs(r[key]) for r in rs if r[key] is not None]
            return round(max(v), 3) if v else None
        out["t"].append(start + (b + 0.5) * bucket)
        out["fsr1"].append(avg(units[1][1]))
        out["fsr2"].append(avg(units[2][1]))
        out["temp"].append(avg("temp_c"))
        out["delta"].append(avg("wrist_delta"))
        out["max_delta"].append(mx("win_max_delta"))
        out["deviation"].append(mx("deviation"))
        out["events"].append(ev.get(b, 0))
    out["bucket_s"] = bucket
    out["fsr_units"] = [units[1][2], units[2][2]]
    return out


# ====================================================================== web server
class App:
    def __init__(self, db, settings, calibration, processor, link, simulate):
        self.db, self.settings, self.cal = db, settings, calibration
        self.processor, self.link, self.simulate = processor, link, simulate
        self._cache = None
        self.calibration_token = secrets.token_urlsafe(32)

    def window(self, query):
        hours = float(query.get("hours", ["24"])[0])
        end = time.time()
        if hours <= 0:
            _, first, _ = self.db.count()
            return (first or end), end
        return end - hours * 3600, end

    def analysis(self, query):
        start, end = self.window(query)
        key = (round(start / 5), round(end / 5))
        if self._cache and self._cache[0] == key:
            return self._cache[1]
        result = analyze(self.db.rows(start, end), self.settings.all(), start, end, self.db.markers(start, end))
        self._cache = (key, result)
        return result


class Handler(BaseHTTPRequestHandler):
    app = None
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, ctype="application/json", extra=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(clean(body), allow_nan=False, default=str).encode("utf-8")
        elif isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n).decode("utf-8")) if n else {}

    def _calibration_unlocked(self):
        supplied = self.headers.get("X-Calibration-Token", "")
        return hmac.compare_digest(supplied.encode(), self.app.calibration_token.encode())

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        app = self.app
        try:
            if url.path in ("/", "/index.html"):
                return self._static("index.html")
            if url.path.startswith("/static/"):
                return self._static(url.path[len("/static/"):])
            if url.path == "/api/state":
                st = app.link.snapshot()
                n, first, last = app.db.count()
                unlocked = self._calibration_unlocked()
                return self._send(200, {
                    "simulate": app.simulate, "now": time.time(), "settings": app.settings.all(),
                    "ble_available": ble_available(),
                    "connected": st["connected"], "last_ok": st["last_ok"], "error": st["error"],
                    "device": st["device"], "resolved_ip": st["resolved_ip"],
                    "rows_received": st["rows_received"], "rows_missed": st["rows_missed"],
                    "live": app.processor.snapshot(), "calibration_unlocked": unlocked,
                    "calibration": app.cal.all() if unlocked else None,
                    "capture": app.processor.capture_status() if unlocked else None,
                    "stored_seconds": n, "first_ts": first, "last_ts": last})
            if url.path == "/api/history":
                start, end = app.window(q)
                data = downsample(app.db.rows(start, end), start, end)
                data.update(markers=app.db.markers(start, end), start=start, end=end)
                return self._send(200, data)
            if url.path == "/api/analysis":
                return self._send(200, app.analysis(q))
            if url.path == "/api/calibration/status":
                if not self._calibration_unlocked():
                    return self._send(401, {"error": "Calibration is locked"})
                return self._send(200, app.processor.capture_status())
            if url.path == "/api/export.csv":
                start, end = app.window(q)
                buf = io.StringIO()
                w = csv.writer(buf)
                w.writerow(["time"] + FIELDS)
                for r in app.db.rows(start, end):
                    w.writerow([time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(r["ts"]))] + [r[f] for f in FIELDS])
                name = time.strftime("smartsplint_%Y%m%d_%H%M.csv")
                return self._send(200, buf.getvalue(), "text/csv; charset=utf-8",
                                  {"Content-Disposition": f'attachment; filename="{name}"'})
            self._send(404, {"error": "not found"})
        except Exception as exc:
            self._send(500, {"error": f"{type(exc).__name__}: {exc}"})

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        app = self.app
        try:
            body = self._body()
            if url.path == "/api/calibration/unlock":
                password = body.get("password")
                if not isinstance(password, str) or not hmac.compare_digest(password.encode(), b"1234"):
                    return self._send(401, {"error": "Incorrect calibration password"})
                return self._send(200, {"token": app.calibration_token})
            if url.path.startswith("/api/calibration/") and not self._calibration_unlocked():
                return self._send(401, {"error": "Calibration is locked"})
            if url.path == "/api/settings":
                if "transport" in body and body["transport"] != "ble":
                    return self._send(400, {"error": "The splint uses Bluetooth only"})
                app._cache = None
                return self._send(200, app.settings.update(body))
            if url.path == "/api/calibration/start":
                kind = body.get("kind")
                if kind not in ("imu", "fsr_zero", "fsr_point"):
                    return self._send(400, {"error": "unknown calibration"})
                fsr = None if kind == "imu" else int(body.get("fsr", 1)) - 1
                if fsr is not None and fsr not in (0, 1):
                    return self._send(400, {"error": "fsr must be 1 or 2"})
                force = None
                if kind == "fsr_point":
                    force = float(body.get("force_n", 0))
                    if force <= 0:
                        return self._send(400, {"error": "Enter the weight first"})
                seconds = 3.0 if kind == "imu" else 4.0
                if not app.link.snapshot()["connected"]:
                    return self._send(409, {"error": "The splint is not connected"})
                if app.simulate:
                    sim = app.link.sim
                    if kind == "imu":
                        sim.still_until = sim.t_ms / 1000 + seconds + 2
                        sim.event = None
                    else:
                        sim.force_override[fsr] = (0.0 if kind == "fsr_zero" else force, time.time() + seconds + 1.0)
                app.processor.start_capture(kind, seconds, fsr, force)
                return self._send(200, {"ok": True, "seconds": seconds})
            if url.path == "/api/calibration/clear":
                part = body.get("part")
                if part == "imu":
                    app.cal.set("imu", None)
                elif part in ("fsr1", "fsr2"):
                    lst = app.cal.get("fsr") or [None, None]
                    lst[int(part[-1]) - 1] = None
                    app.cal.set("fsr", lst)
                else:
                    return self._send(400, {"error": "unknown part"})
                app.db.add_marker("calibration", f"Calibration cleared: {part}")
                return self._send(200, {"ok": True})
            if url.path == "/api/calibration/remove_point":
                k, raw = int(body["fsr"]) - 1, float(body["raw"])
                lst = app.cal.get("fsr") or [None, None]
                if lst[k]:
                    lst[k]["points"] = [p for p in lst[k]["points"] if abs(p["raw"] - raw) > 1e-6]
                    app.cal.set("fsr", lst)
                return self._send(200, {"ok": True})
            if url.path == "/api/baseline":
                now = time.time()
                app.settings.update({"baseline_ts": now})
                app.db.add_marker("baseline", "Baseline started")
                app._cache = None
                return self._send(200, {"ok": True, "baseline_ts": now})
            if url.path == "/api/marker":
                text = str(body.get("text", "")).strip()[:200]
                if not text:
                    return self._send(400, {"error": "Write a note first"})
                app.db.add_marker("note", text)
                return self._send(200, {"ok": True})
            self._send(404, {"error": "not found"})
        except Exception as exc:
            self._send(500, {"ok": False, "error": f"{type(exc).__name__}: {exc}"})

    def _static(self, name):
        path = (STATIC_DIR / name).resolve()
        if STATIC_DIR.resolve() not in path.parents or not path.is_file():
            return self._send(404, {"error": "not found"})
        ctype = {".html": "text/html; charset=utf-8", ".js": "application/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml"}.get(path.suffix, "application/octet-stream")
        self._send(200, path.read_bytes(), ctype)


# ====================================================================== demo history
def create_demo_history(db, calibration, days):
    """Simulated per-second history (one row every 5 s) plus a demo calibration, for testing the analysis."""
    sim = Simulator(seed=7)
    now = time.time()
    start = now - days * 86400
    zero = round(sim_fsr_raw(0.0) + 3, 1)
    points = [{"raw": round(sim_fsr_raw(f), 1), "force_n": f} for f in (0.5, 1.0, 2.0, 4.0, 8.0)]
    if not calibration.get("imu"):
        calibration.set("imu", {"gyro_bias_dps": Simulator.BIAS, "reference_deg": 25.0, "time": start,
                                "demo": True})
    if not any(calibration.get("fsr") or []):
        calibration.set("fsr", [{"zero_raw": zero, "points": points, "time": start, "demo": True},
                                {"zero_raw": zero, "points": points, "time": start, "demo": True}])
    cal = calibration.get("fsr")
    rows, t, events = [], start, 0
    rng = random.Random(11)
    while t < now - 5:
        days_in = (t - start) / 86400
        sim.event = None
        rate = (4.0 + 3.0 * days_in) / 3600 * 5
        moving = rng.random() < rate
        if moving:
            events += 1
        _, _, _, forces, temp, _ = sim.truth(t, days_in)
        if moving:
            forces = [f * 1.3 for f in forces]
        raws = [sim_fsr_raw(f) + rng.gauss(0, 4) for f in forces]
        delta = rng.uniform(7, 16) * rng.choice([-1, 1]) if moving else rng.gauss(0, 0.5)
        rows.append({"ts": t, "fsr1_raw": raws[0], "fsr2_raw": raws[1],
                     "fsr1_kpa": fsr_pressure(raws[0], cal[0]), "fsr2_kpa": fsr_pressure(raws[1], cal[1]),
                     "temp_c": round(temp + rng.gauss(0, 0.05), 2), "theta_f": rng.uniform(-20, 30),
                     "theta_h": None, "wrist_delta": delta, "deviation": rng.uniform(-8, 8) if moving else 0.0,
                     "omega_rel": rng.uniform(20, 70) if moving else abs(rng.gauss(0, 1)),
                     "win_max_delta": abs(delta), "win_max_omega": rng.uniform(20, 70) if moving else 1.0,
                     "events_total": events, "in_event": int(moving), "imu_f_ok": 1, "imu_h_ok": 1,
                     "rssi": -55, "calibrated": 1, "n_samples": 50, "n_missed": 0})
        t += 5
        if len(rows) >= 5000:
            db.insert_many(rows)
            rows = []
    db.insert_many(rows)
    db.add_marker("note", f"Simulated demo history ({days:g} days)", ts=start)


# ====================================================================== main
def main():
    parser = argparse.ArgumentParser(description="SmartSplint Monitor")
    parser.add_argument("--port", type=int, default=8000, help="dashboard port (default 8000)")
    parser.add_argument("--simulate", action="store_true", help="run without hardware using simulated raw data")
    parser.add_argument("--demo-days", type=float, default=0, help="with --simulate: create N days of history")
    parser.add_argument("--no-browser", action="store_true", help="do not open the browser automatically")
    parser.add_argument("--data-dir", help="folder for the database, settings and calibration (default ./data)")
    args = parser.parse_args()

    data_dir = Path(args.data_dir).resolve() if args.data_dir else DATA_DIR
    data_dir.mkdir(parents=True, exist_ok=True)
    suffix = "_simulation" if args.simulate else ""
    db = Database(data_dir / f"splint{suffix}.db")
    settings = Settings(data_dir / f"settings{suffix}.json", DEFAULT_SETTINGS)
    calibration = JsonStore(data_dir / f"calibration{suffix}.json", EMPTY_CALIBRATION)
    if settings.get("transport") != "ble":
        settings.update({"transport": "ble"})
    if args.simulate and args.demo_days > 0 and db.count()[0] == 0:   # only once, into an empty database
        print(f"Creating {args.demo_days:g} days of simulated history...")
        create_demo_history(db, calibration, args.demo_days)

    processor = Processor(calibration, settings)
    link = DeviceLink(db, settings, processor, simulate=args.simulate)
    link.start()
    Handler.app = App(db, settings, calibration, processor, link, args.simulate)
    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = f"http://localhost:{args.port}"
    mode = "SIMULATION MODE (no hardware)" if args.simulate else f"Bluetooth: {settings.get('ble_name')}"
    print(f"SmartSplint Monitor - {mode}")
    print(f"Dashboard: {url}    (press Ctrl+C to stop)")
    print(f"Data folder: {data_dir}")
    if not args.no_browser:
        threading.Timer(1.0, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
