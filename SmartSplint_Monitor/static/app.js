/* SmartSplint Monitor - dashboard logic (no external libraries, works offline). */
// FSR 400 active area (5.08 mm diameter) in m², used to show calibration points as pressure.
const FSR_AREA_M2 = Math.PI * (5.08e-3 / 2) ** 2;
const $ = (id) => document.getElementById(id);
const fmt = (v, d = 1) => {
  if (v === null || v === undefined || Number.isNaN(v)) return "--";
  const s = Number(v).toFixed(d);
  return /^-0(\.0+)?$/.test(s) ? s.slice(1) : s;   // show "0" instead of "-0"
};
let state = null, history = null, analysis = null, lastCalJson = "", settingsLoaded = false;
let range = 24;
let calibrationToken = "";

// ------------------------------------------------------------------ helpers
async function api(path, body) {
  const headers = {};
  if (calibrationToken && (path === "/api/state" || path.startsWith("/api/calibration/")))
    headers["X-Calibration-Token"] = calibrationToken;
  const opts = body === undefined ? {} : {
    method: "POST", body: JSON.stringify(body)
  };
  if (body !== undefined) headers["Content-Type"] = "application/json";
  const r = await fetch(path, { cache: "no-store", headers, ...opts });
  if (r.status === 401 && headers["X-Calibration-Token"] === calibrationToken) {
    calibrationToken = "";
    if (!$("tab-calibration").hidden) showTab("settings");
  }
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(data.error || data.message || `HTTP ${r.status}`);
  return data;
}

let toastTimer = null;
function toast(msg, kind = "") {
  const t = $("toast");
  t.textContent = msg;
  t.className = "toast " + kind;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.hidden = true), 4500);
}

function ago(ts) {
  if (!ts) return "never";
  const s = Date.now() / 1000 - ts;
  if (s < 90) return "just now";
  if (s < 5400) return `${Math.round(s / 60)} min ago`;
  if (s < 129600) return `${Math.round(s / 3600)} h ago`;
  return `${Math.round(s / 86400)} days ago`;
}
const timeStr = (ts) => new Date(ts * 1000).toLocaleString([], { dateStyle: "medium", timeStyle: "short" });
const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
const levelIcon = { ok: "😊", warning: "⚠️", info: "💡" };
const escapeHtml = (s) => String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

// ------------------------------------------------------------------ tabs and range
function showTab(name) {
  if (!["measurements", "settings", "calibration"].includes(name)) name = "measurements";
  if (name === "calibration" && !calibrationToken) name = "settings";
  if (name !== "calibration") {
    calibrationToken = "";
    lastCalJson = "";
  }
  document.querySelectorAll("#tabs button").forEach((b) => {
    const active = b.dataset.tab === (name === "calibration" ? "settings" : name);
    b.classList.toggle("active", active);
    if (active) b.setAttribute("aria-current", "page"); else b.removeAttribute("aria-current");
  });
  document.querySelectorAll(".tab").forEach((s) => (s.hidden = s.id !== "tab-" + name));

}
document.querySelectorAll("#tabs button").forEach((b) => b.addEventListener("click", () => showTab(b.dataset.tab)));
$("range").addEventListener("change", () => {
  range = Number($("range").value);
  try { localStorage.setItem("range", range); } catch (e) { /* ignore */ }
  refreshData();
});

// ------------------------------------------------------------------ live state (every second)
async function pollState() {
  try {
    const tokenAtRequest = calibrationToken;
    state = await api("/api/state");
    renderConnection();
    renderLive();
    if (tokenAtRequest && tokenAtRequest === calibrationToken && !$("tab-calibration").hidden) {
      if (state.calibration_unlocked) renderCalibration();
      else showTab("settings");
    }
    if (!settingsLoaded) loadSettingsForm();
  } catch (e) {
    $("connPill").classList.remove("on");
    $("connText").textContent = "App server stopped";
  } finally {
    setTimeout(pollState, 1000);
  }
}

function renderConnection() {
  const s = state, pill = $("connPill");
  $("simBanner").hidden = !s.simulate;
  pill.classList.toggle("on", s.connected);
  let text;
  if (s.simulate) text = s.connected ? "Simulation running" : "Simulation starting…";
  else if (s.connected) text = "Connected by Bluetooth";
  else text = "Bluetooth: not connected";
  $("connText").textContent = text;
  pill.title = s.error || "";
  $("patientLine").textContent = s.settings.patient_label
    ? `👤 ${s.settings.patient_label}` : "Smart orthopedic splint · remote monitoring";
  const d = s.device || {};
  $("connDetail").innerHTML = s.connected
    ? `✅ ${escapeHtml(d.firmware || "")} · samples received: ${s.rows_received.toLocaleString()} · missed: ${s.rows_missed}`
    : `⏳ ${escapeHtml(s.error || "Not connected")}`;
}

function renderLive() {
  const L = state.live || {};
  const ok = state.connected && L.ts;
  for (const k of [0, 1]) {
    const n = L.fsr_kpa ? L.fsr_kpa[k] : null, raw = L.fsr_raw ? L.fsr_raw[k] : null;
    const big = $(k ? "liveF2" : "liveF1"), small = $(k ? "liveF2s" : "liveF1s");
    if (!ok || raw == null) {
      big.textContent = "--";
      small.textContent = "No current reading";
      continue;
    }
    if (n !== null && n !== undefined) {
      big.innerHTML = `${fmt(n, 1)}<span class="u">kPa</span>`;
      small.textContent = `≈ ${fmt(n * 1000, 0)} Pa · raw ${raw}`;
    } else {
      big.textContent = raw;
      small.textContent = "Raw value · not calibrated";
    }
  }
  const temp = L.temp_c ?? (state.device || {}).temp_c;
  $("liveT").innerHTML = ok && temp != null ? `${fmt(temp, 1)}<span class="u">°C</span>` : "--";
  const d = L.wrist_delta;
  const wCard = $("liveW").closest(".card");
  if (ok && d != null) {
    $("liveW").innerHTML = `${d > 0 ? "+" : ""}${fmt(d, 1)}<span class="u">°</span>`;
    $("liveWs").textContent = L.in_event ? "🔴 moving now" : "from the fitted position";
    const clamped = Math.max(-20, Math.min(20, d));
    $("meterNeedle").style.left = `calc(${50 + clamped * 2.5}% - 2px)`;
  } else {
    $("liveW").textContent = "--";
    $("liveWs").textContent = ok ? "Calibrate the motion sensors" : "from the fitted position";
  }
  wCard.classList.toggle("moving", !!(ok && L.in_event));
  $("liveD").innerHTML = ok && L.calibrated && L.imu_ok?.every(Boolean) && L.deviation != null ? `${fmt(Math.abs(L.deviation), 1)}<span class="u">°</span>` : "--";
  const m = analysis && analysis.movement;
  $("liveC").innerHTML = m && m.compliance_pct != null ? `${fmt(m.compliance_pct, 1)}<span class="u">%</span>` : "--";
  $("liveCs").textContent = m && m.events != null ? `${m.events} movements in the selected period` : "compliance in the selected period";

  const rawImus = $("liveImuRaw");
  rawImus.hidden = !ok || L.calibrated || !L.imu_raw?.some(Boolean);
  rawImus.textContent = rawImus.hidden ? "" : L.imu_raw.map((axes, i) =>
    (i ? "Hand" : "Forearm") + ": " + (axes
      ? "Accel " + axes.slice(0, 3).join(", ") + " · Gyro " + axes.slice(3).join(", ")
      : "No data")).join(" | ");
  // raw readings on the calibration tab
  const imu = L.imu_ok || [false, false];
  $("rawGrid").innerHTML = [
    ["FSR 1 raw", L.fsr_raw ? L.fsr_raw[0] : "--"], ["FSR 2 raw", L.fsr_raw ? L.fsr_raw[1] : "--"],
    ["Forearm angle", `${fmt(L.theta_f, 1)}°`], ["Hand angle", `${fmt(L.theta_h, 1)}°`],
    ["Forearm IMU", imu[0] ? "✅ OK" : "❌ No data"], ["Hand IMU", imu[1] ? "✅ OK" : "❌ No data"],
    ["Temperature", `${fmt(temp, 2)} °C`], ["Movements this session", L.events_session ?? "--"],
  ].map(([k, v]) => `<div>${k}<b>${v}</b></div>`).join("");
}

// ------------------------------------------------------------------ history + alerts (every 10 s)
// /api/analysis supplies the alerts, the status banner and the compliance card on Measurements.
async function refreshData() {
  try {
    [history, analysis] = await Promise.all([api(`/api/history?hours=${range}`), api(`/api/analysis?hours=${range}`)]);
    renderAlerts();
    $("exportBtn").href = `/api/export.csv?hours=${range}`;
  } catch (e) {
    console.warn(e);
  }
}

function renderAlerts() {
  const alerts = (analysis && analysis.alerts) || [];
  // the Alerts panel appears only when at least one alert (warning) has occurred
  const warnings = alerts.filter((a) => a.level === "warning");
  const html = warnings.map((a) => `<div class="alert ${a.level}"><div class="a-icon">${levelIcon[a.level] || "💡"}</div>
    <div><div class="a-title">${escapeHtml(a.title)}</div><div class="a-detail">${escapeHtml(a.detail)}</div></div></div>`).join("");
  $("alertList").innerHTML = html;
  $("alertPanel").hidden = warnings.length === 0;
  // the status banner is shown only when there is no alert, so the same message is not repeated
  $("hero").hidden = warnings.length > 0;
  const top = alerts.find((a) => a.level === "warning") || alerts[0];
  const hero = $("hero");
  hero.className = "hero " + (top ? top.level : "");
  $("heroEmoji").textContent = top ? (top.level === "ok" ? "😊" : top.level === "warning" ? "🩺" : "💡") : "⏳";
  $("heroTitle").textContent = top ? top.title : "Waiting for data";
  $("heroDetail").textContent = top
    ? top.detail
    : "Connect the splint to start monitoring.";
}

// ------------------------------------------------------------------ calibration
function renderCalibration() {
  const cal = state.calibration || {}, cap = state.capture || {};
  const key = JSON.stringify(cal) + JSON.stringify(cap.running ? [cap.kind, cap.fsr] : null);
  // progress bars always update; panels only re-render when something changed
  const prog = (id, show, p) => { const el = $(id); if (!el) return; el.hidden = !show; el.firstElementChild.style.width = `${Math.round((p || 0) * 100)}%`; };
  prog("calImuProg", cap.running && cap.kind === "imu", cap.progress);
  [0, 1].forEach((k) => prog(`calFsrProg${k}`, cap.running && cap.kind !== "imu" && cap.fsr === k, cap.progress));
  const live = state.live || {};
  [0, 1].forEach((k) => { const el = $(`fsrLive${k}`); if (el) el.textContent = live.fsr_raw ? live.fsr_raw[k] : "--"; });
  if (key === lastCalJson) return;
  lastCalJson = key;

  const imu = cal.imu;
  const st = $("calImuStatus");
  st.className = "cal-status" + (imu ? " done" : "");
  st.textContent = imu ? `✅ Calibrated ${ago(imu.time)}${imu.demo ? " (demo)" : ""}` : "⚠️ Not calibrated";
  $("calImuDetails").innerHTML = imu
    ? `Fitted wrist angle: <b>${fmt(imu.reference_deg, 1)}°</b><br>Gyro offsets (°/s) – forearm: ${imu.gyro_bias_dps[0].map((v) => fmt(v, 2)).join(", ")}
       · hand: ${imu.gyro_bias_dps[1].map((v) => fmt(v, 2)).join(", ")}`
    : "Without calibration the wrist movement cannot be measured.";
  $("calImuBtn").disabled = !!cap.running;

  [0, 1].forEach((k) => {
    const panel = $(`calFsr${k + 1}`);
    const keepGrams = $(`grams${k}`) ? $(`grams${k}`).value : "";
    const c = (cal.fsr || [])[k];
    const pts = c && c.points ? c.points : [];
    const done = c && c.zero_raw != null && pts.length;
    panel.innerHTML = `
      <h2>${k ? "⌚ FSR 2 · wrist" : "🖐️ FSR 1 · back of hand"}</h2>
      <ol class="steps">
        <li>Remove all load from the sensor and press <b>Record zero</b>.</li>
        <li>Place a known weight on the sensing area (use a small flat disc so the weight rests on the sensor),
          enter its mass and press <b>Record weight</b>.</li>
        <li>Repeat with 2–4 different weights, from light to heavy.</li>
      </ol>
      <div class="cal-status ${done ? "done" : ""}">${done ? `✅ Calibrated with ${pts.length} point${pts.length > 1 ? "s" : ""} · ${ago(c.time)}${c.demo ? " (demo)" : ""}` : "⚠️ Not calibrated – showing raw readings"}</div>
      <div class="cal-details">Live raw reading: <b id="fsrLive${k}">--</b>${c && c.zero_raw != null ? ` · zero: <b>${fmt(c.zero_raw, 0)}</b>` : ""}</div>
      <div class="progress" id="calFsrProg${k}" hidden><div></div></div>
      <div class="btn-row"><button class="btn" data-zero="${k + 1}" ${cap.running ? "disabled" : ""}>⓪ Record zero</button></div>
      <div class="weight-row">
        <label>Known weight (grams)<input id="grams${k}" type="number" min="1" step="1" placeholder="e.g. 200"></label>
        <button class="btn" data-weight="${k + 1}" ${cap.running ? "disabled" : ""}>⚖️ Record weight</button>
      </div>
      ${pts.length ? `<table class="points"><tr><th>Weight</th><th>Pressure</th><th>Reading</th><th></th></tr>
        ${pts.map((p) => `<tr><td>${fmt(p.force_n / 9.80665 * 1000, 0)} g</td><td>${fmt(p.force_n / FSR_AREA_M2 / 1000, 1)} kPa</td><td>${fmt(p.raw, 0)}</td>
          <td><button class="btn ghost small-btn" data-rmfsr="${k + 1}" data-raw="${p.raw}">✕</button></td></tr>`).join("")}</table>` : ""}
      <div class="btn-row"><button class="btn ghost" data-clear="fsr${k + 1}">Reset FSR ${k + 1}</button></div>`;
    $(`grams${k}`).value = keepGrams;
  });
}

async function runCapture(body, label) {
  try {
    await api("/api/calibration/start", body);
    toast(`${label}… keep still`, "");
    let st;
    do {
      await new Promise((r) => setTimeout(r, 300));
      st = await api("/api/calibration/status");
    } while (st.running);
    const res = st.result || { ok: false, message: "No result" };
    toast((res.ok ? "✅ " : "⚠️ ") + res.message, res.ok ? "good" : "bad");
  } catch (e) {
    toast("⚠️ " + e.message, "bad");
  }
  lastCalJson = "";
}

$("calImuBtn").addEventListener("click", () => runCapture({ kind: "imu" }, "Calibrating motion sensors"));
document.addEventListener("click", async (e) => {
  const b = e.target.closest("button");
  if (!b) return;
  if (b.dataset.zero) runCapture({ kind: "fsr_zero", fsr: Number(b.dataset.zero) }, `Recording FSR ${b.dataset.zero} zero`);
  if (b.dataset.weight) {
    const k = Number(b.dataset.weight), grams = Number($(`grams${k - 1}`).value);
    if (!grams || grams <= 0) { toast("⚠️ Enter the weight in grams first", "bad"); return; }
    runCapture({ kind: "fsr_point", fsr: k, force_n: grams / 1000 * 9.80665 }, `Recording ${grams} g on FSR ${k}`);
  }
  if (b.dataset.clear) {
    if (!confirm(`Reset the ${b.dataset.clear.toUpperCase()} calibration? This cannot be undone.`)) return;
    await api("/api/calibration/clear", { part: b.dataset.clear });
    toast("Calibration reset");
    lastCalJson = "";
  }
  if (b.dataset.rmfsr) {
    await api("/api/calibration/remove_point", { fsr: Number(b.dataset.rmfsr), raw: Number(b.dataset.raw) });
    lastCalJson = "";
  }
});

// ------------------------------------------------------------------ settings
const LIMITS = [
  ["pressure_rise_pct", "Pressure rise that raises an alert", "%"],
  ["pressure_rise_min_kpa", "…and at least (calibrated)", "kPa"],
  ["pressure_rise_min_raw", "…and at least (not calibrated)", "raw"],
  ["temp_rise_c", "Temperature rise that raises an alert", "°C"],
  ["sustain_minutes", "A rise must last at least", "min"],
  ["baseline_minutes", "Length of the baseline period", "min"],
  ["movement_angle_deg", "Wrist movement that counts as an event", "°"],
  ["deviation_angle_deg", "Sideways movement that counts as an event", "°"],
  ["events_per_hour_limit", "Movements per hour before an alert", "/h"],
  ["compliance_min_pct", "Minimum compliance", "%"],
  ["loosening_ratio", "Movement increase that suggests a loose splint", "×"],
];
$("limitsGrid").innerHTML = LIMITS.map(([k, label, u]) =>
  `<label>${label} (${u})<input id="lim_${k}" type="number" step="any"></label>`).join("");

function loadSettingsForm() {
  const s = state.settings;
  $("setBle").value = s.ble_name || "";
  $("setPatient").value = s.patient_label || "";
  LIMITS.forEach(([k]) => ($(`lim_${k}`).value = s[k]));
  $("transportHint").textContent = state.ble_available ? "Bluetooth Low Energy" : "Install the Bluetooth package: python -m pip install -r requirements.txt";
  $("baselineInfo").textContent = s.baseline_ts ? `Current baseline started ${timeStr(s.baseline_ts)}.` : "No baseline set: the start of the selected period is used.";
  settingsLoaded = true;
}

$("openCalibration").addEventListener("click", () => {
  $("calibrationLogin").reset();
  $("calibrationError").textContent = "";
  $("calibrationDialog").showModal();
  $("calibrationPassword").focus();
});
$("cancelCalibration").addEventListener("click", () => $("calibrationDialog").close());
$("closeCalibration").addEventListener("click", () => showTab("settings"));
$("calibrationLogin").addEventListener("submit", async (event) => {
  event.preventDefault();
  try {
    const response = await api("/api/calibration/unlock", {password: $("calibrationPassword").value});
    if (!$("calibrationDialog").open) return;
    calibrationToken = response.token;
    $("calibrationPassword").value = "";
    $("calibrationDialog").close();
    showTab("calibration");
  } catch (e) {
    $("calibrationError").textContent = e.message;
  }
});

$("saveSettings").addEventListener("click", async () => {
  const body = { ble_name: $("setBle").value, patient_label: $("setPatient").value };
  for (const [k] of LIMITS) {
    const v = $(`lim_${k}`).value;
    if (v === "" || Number.isNaN(Number(v))) { toast(`⚠️ Please enter a number for every limit`, "bad"); return; }
    body[k] = Number(v);
  }
  try {
    await api("/api/settings", body);
    toast("✅ Settings saved", "good");
    settingsLoaded = false;
    refreshData();
  } catch (e) { toast("⚠️ " + e.message, "bad"); }
});
$("setPatient").addEventListener("change", () => api("/api/settings", { patient_label: $("setPatient").value }).catch(() => {}));

$("baselineBtn").addEventListener("click", async () => {
  await api("/api/baseline", {});
  toast("⏱ New baseline started", "good");
  settingsLoaded = false;
  refreshData();
});


// ------------------------------------------------------------------ start
try {
  const r = localStorage.getItem("range");
  if (r !== null) { range = Number(r); $("range").value = r; }
} catch (e) { /* storage unavailable */ }
showTab("measurements");
pollState();
refreshData();
setInterval(refreshData, 10000);
