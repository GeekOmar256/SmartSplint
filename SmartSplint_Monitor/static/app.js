/* SmartSplint Monitor - dashboard logic (no external libraries, works offline). */
const $ = (id) => document.getElementById(id);
const fmt = (v, d = 1) => {
  if (v === null || v === undefined || Number.isNaN(v)) return "--";
  const s = Number(v).toFixed(d);
  return /^-0(\.0+)?$/.test(s) ? s.slice(1) : s;   // show "0" instead of "-0"
};
let state = null, history = null, analysis = null, lastCalJson = "", settingsLoaded = false;
let range = 24;

// ------------------------------------------------------------------ helpers
async function api(path, body) {
  const opts = body === undefined ? {} : {
    method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body)
  };
  const r = await fetch(path, { cache: "no-store", ...opts });
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
  document.querySelectorAll("#tabs button").forEach((b) => b.classList.toggle("active", b.dataset.tab === name));
  document.querySelectorAll(".tab").forEach((s) => (s.hidden = s.id !== "tab-" + name));
  try { localStorage.setItem("tab", name); } catch (e) { /* storage unavailable */ }
  if (name === "charts") drawCharts();
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
    state = await api("/api/state");
    renderConnection();
    renderLive();
    renderCalibration();
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
  const tr = s.settings.transport;
  let text;
  if (s.simulate) text = s.connected ? "Simulation running" : "Simulation starting…";
  else if (s.connected) text = tr === "ble" ? "Connected by Bluetooth" : `Connected by Wi-Fi · ${s.resolved_ip || s.settings.device_host}`;
  else text = tr === "ble" ? "Bluetooth: not connected" : "Wi-Fi: not connected";
  $("connText").textContent = text;
  pill.title = s.error || "";
  $("patientLine").textContent = s.settings.patient_label
    ? `👤 ${s.settings.patient_label}` : "Smart orthopedic splint · remote monitoring";
  const d = s.device || {};
  $("connDetail").innerHTML = s.connected
    ? `✅ ${escapeHtml(d.firmware || "")} · samples received: ${s.rows_received.toLocaleString()} · missed: ${s.rows_missed}` +
      (d.rssi ? ` · Wi-Fi signal ${d.rssi} dBm` : "")
    : `⏳ ${escapeHtml(s.error || "Not connected")}`;
}

function renderLive() {
  const L = state.live || {};
  const ok = state.connected && L.ts;
  for (const k of [0, 1]) {
    const n = L.fsr_n ? L.fsr_n[k] : null, raw = L.fsr_raw ? L.fsr_raw[k] : null;
    const big = $(k ? "liveF2" : "liveF1"), small = $(k ? "liveF2s" : "liveF1s");
    if (!ok || raw === null || raw === undefined) { big.textContent = "--"; continue; }
    if (n !== null && n !== undefined) {
      big.innerHTML = `${fmt(n, 2)}<span class="u">N</span>`;
      small.textContent = `≈ ${fmt(n * 1000 / 9.80665, 0)} g · raw ${raw}`;
    } else {
      big.innerHTML = `${raw}<span class="u">ADC</span>`;
      small.textContent = "Not calibrated – see Calibration";
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
  $("liveD").innerHTML = ok && L.deviation != null ? `${fmt(Math.abs(L.deviation), 1)}<span class="u">°</span>` : "--";
  const m = analysis && analysis.movement;
  $("liveC").innerHTML = m && m.compliance_pct != null ? `${fmt(m.compliance_pct, 1)}<span class="u">%</span>` : "--";
  $("liveCs").textContent = m && m.events != null ? `${m.events} movements in the selected period` : "compliance in the selected period";

  // raw readings on the calibration tab
  const imu = L.imu_ok || [false, false];
  $("rawGrid").innerHTML = [
    ["FSR 1 raw", L.fsr_raw ? L.fsr_raw[0] : "--"], ["FSR 2 raw", L.fsr_raw ? L.fsr_raw[1] : "--"],
    ["Forearm angle", `${fmt(L.theta_f, 1)}°`], ["Hand angle", `${fmt(L.theta_h, 1)}°`],
    ["Forearm IMU", imu[0] ? "✅ OK" : "❌ No data"], ["Hand IMU", imu[1] ? "✅ OK" : "❌ No data"],
    ["Temperature", `${fmt(temp, 2)} °C`], ["Movements this session", L.events_session ?? "--"],
  ].map(([k, v]) => `<div>${k}<b>${v}</b></div>`).join("");
}

// ------------------------------------------------------------------ history + analysis (every 10 s)
async function refreshData() {
  try {
    [history, analysis] = await Promise.all([api(`/api/history?hours=${range}`), api(`/api/analysis?hours=${range}`)]);
    renderAlerts();
    renderAnalysis();
    renderNotes();
    drawCharts();
    $("exportBtn").href = `/api/export.csv?hours=${range}`;
  } catch (e) {
    console.warn(e);
  }
}

function renderAlerts() {
  const alerts = (analysis && analysis.alerts) || [];
  const html = alerts.map((a) => `<div class="alert ${a.level}"><div class="a-icon">${levelIcon[a.level] || "💡"}</div>
    <div><div class="a-title">${escapeHtml(a.title)}</div><div class="a-detail">${escapeHtml(a.detail)}</div></div></div>`).join("");
  $("alertList").innerHTML = html || '<div class="hint">No data yet.</div>';
  $("alertList2").innerHTML = $("alertList").innerHTML;
  const top = alerts.find((a) => a.level === "warning") || alerts[0];
  const hero = $("hero");
  hero.className = "hero " + (top ? top.level : "");
  $("heroEmoji").textContent = top ? (top.level === "ok" ? "😊" : top.level === "warning" ? "🩺" : "💡") : "⏳";
  $("heroTitle").textContent = top ? top.title : "Waiting for data";
  const nWarn = alerts.filter((a) => a.level === "warning").length;
  $("heroDetail").textContent = top
    ? (nWarn > 1 ? `${top.detail} (${nWarn} findings need attention – see Alerts.)` : top.detail)
    : "Connect the splint to start monitoring.";
}

function renderAnalysis() {
  const A = analysis;
  if (!A || A.empty) { $("analysisCards").innerHTML = '<div class="hint">No data in this period yet.</div>'; return; }
  const row = (k, v) => `<div class="row"><span>${k}</span><span>${v}</span></div>`;
  const badge = (flag, okText, badText) => `<span class="badge ${flag ? "warning" : "ok"}">${flag ? badText : okText}</span>`;
  const cards = [];
  A.pressure.forEach((p, i) => {
    const u = p.unit, d = u === "N" ? 2 : 0;
    cards.push(`<div class="card ${i ? "c-pink" : "c-orange"}"><div class="card-head"><span class="icon">${i ? "⌚" : "🖐️"}</span>${escapeHtml(p.sensor)}</div>
      ${row("Baseline", `${fmt(p.baseline, d)} ${u}`)}${row("Recent", `${fmt(p.recent, d)} ${u}`)}
      ${row("Change", p.change_pct == null ? "--" : `${p.change_pct > 0 ? "+" : ""}${fmt(p.change_pct, 0)} %`)}
      ${row("Trend", `${fmt(p.slope_per_h, d + 1)} ${u}/h`)}${row("Maximum", `${fmt(p.max, d)} ${u}`)}
      ${badge(p.flag, "Stable", "Sustained rise")}${p.note ? `<div class="small">${escapeHtml(p.note)}</div>` : ""}</div>`);
  });
  const T = A.temperature;
  cards.push(`<div class="card c-red"><div class="card-head"><span class="icon">🌡️</span>Skin temperature</div>
    ${row("Baseline", `${fmt(T.baseline, 1)} °C`)}${row("Recent", `${fmt(T.recent, 1)} °C`)}
    ${row("Change", `${T.change > 0 ? "+" : ""}${fmt(T.change, 1)} °C`)}${row("Range", `${fmt(T.min, 1)} – ${fmt(T.max, 1)} °C`)}
    ${row("Trend", `${fmt(T.slope_per_h, 2)} °C/h`)}${badge(T.flag, "Normal", "Sustained rise")}</div>`);
  const M = A.movement;
  cards.push(`<div class="card c-violet"><div class="card-head"><span class="icon">🔄</span>Wrist movement</div>
    ${row("Movements", M.events)}${row("Per hour", fmt(M.events_per_hour, 1))}
    ${row("Largest movement", `${fmt(M.max_delta, 1)}°`)}${row("Time out of position", `${fmt(M.time_outside_pct, 1)} %`)}
    ${row("Compliance", `${fmt(M.compliance_pct, 1)} %`)}
    ${badge(M.compliance_pct != null && M.compliance_pct < state.settings.compliance_min_pct, "Kept still", "Low compliance")}</div>`);
  const Lo = M.loosening || {};
  cards.push(`<div class="card c-blue"><div class="card-head"><span class="icon">🩹</span>Splint fit</div>
    ${row("Method", Lo.method || "needs ≥ 6 h or 3 days")}${row("Earlier", Lo.first ?? "--")}${row("Latest", Lo.last ?? "--")}
    ${row("Trend", Lo.trend_per_day != null ? `${fmt(Lo.trend_per_day, 2)} per day` : "--")}
    ${badge(Lo.flag, "Fit looks stable", "Possibly loose")}</div>`);
  const MP = A.movement_pressure;
  cards.push(`<div class="card c-green"><div class="card-head"><span class="icon">🔗</span>Movement ↔ pressure</div>
    ${row("Seconds with movement", MP.moving_seconds)}${row("…with a pressure peak", MP.with_pressure_peak)}
    ${row("Share", MP.pct == null ? "--" : `${fmt(MP.pct, 0)} %`)}${row("Correlation", fmt(MP.correlation, 2))}
    <span class="badge info">Explains pressure peaks</span></div>`);
  const Q = A.data_quality;
  cards.push(`<div class="card"><div class="card-head"><span class="icon">📶</span>Data quality</div>
    ${row("Coverage", `${fmt(Q.coverage_pct, 1)} %`)}${row("Gaps > 10 s", Q.gaps)}
    ${row("Raw samples", (Q.samples || 0).toLocaleString())}${row("Missed samples", Q.missed)}
    ${row("Both IMUs OK", `${fmt(Q.imu_ok_pct, 1)} %`)}${row("Baseline", A.baseline ? A.baseline.source : "--")}</div>`);
  $("analysisCards").innerHTML = cards.join("");
}

function renderNotes() {
  const ms = ((history && history.markers) || []).slice().reverse();
  $("noteList").innerHTML = ms.length
    ? ms.map((m) => `<div class="note kind-${m.kind}">${escapeHtml(m.text)}<div class="when">${timeStr(m.ts)}</div></div>`).join("")
    : '<div class="hint">No notes in this period.</div>';
}

// ------------------------------------------------------------------ charts (canvas, no libraries)
function niceStep(span, n) {
  const raw = span / n, p = Math.pow(10, Math.floor(Math.log10(raw))), f = raw / p;
  return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * p;
}

function drawChart(canvas, opt) {
  const dpr = window.devicePixelRatio || 1, w = canvas.clientWidth, h = canvas.clientHeight;
  if (!w) return;
  canvas.width = w * dpr; canvas.height = h * dpr;
  const g = canvas.getContext("2d");
  g.scale(dpr, dpr);
  g.clearRect(0, 0, w, h);
  const P = { l: 52, r: 14, t: 14, b: 30 };
  const t = opt.t || [], x0 = opt.start, x1 = opt.end;
  const vals = opt.series.flatMap((s) => s.data.filter((v) => v != null));
  (opt.thresholds || []).forEach((th) => vals.push(th.y));
  g.font = "12px Segoe UI, system-ui, sans-serif";
  g.fillStyle = css("--muted");
  if (!t.length || !opt.series.some((s) => s.data.some((v) => v != null))) {
    g.textAlign = "center";
    g.fillText("No data in this period yet", w / 2, h / 2);
    canvas._hover = null;
    return;
  }
  let lo = opt.zero ? 0 : Math.min(...vals), hi = Math.max(...vals);
  if (hi - lo < 1e-6) { hi += 1; lo -= opt.zero ? 0 : 1; }
  const padv = (hi - lo) * 0.1; hi += padv; if (!opt.zero) lo -= padv;
  const X = (v) => P.l + (v - x0) / (x1 - x0) * (w - P.l - P.r);
  const Y = (v) => h - P.b - (v - lo) / (hi - lo) * (h - P.t - P.b);
  // grid + y labels
  const step = niceStep(hi - lo, 5);
  g.strokeStyle = css("--chart-grid"); g.lineWidth = 1; g.textAlign = "right"; g.textBaseline = "middle";
  for (let v = Math.ceil(lo / step) * step; v <= hi; v += step) {
    g.beginPath(); g.moveTo(P.l, Y(v)); g.lineTo(w - P.r, Y(v)); g.stroke();
    g.fillText(Math.abs(step) < 1 ? v.toFixed(step < 0.1 ? 2 : 1) : v.toFixed(0), P.l - 8, Y(v));
  }
  // x labels
  g.textAlign = "center"; g.textBaseline = "top";
  const spanH = (x1 - x0) / 3600, nt = Math.max(2, Math.floor(w / 110));
  for (let i = 0; i <= nt; i++) {
    const ts = x0 + (x1 - x0) * i / nt, dt = new Date(ts * 1000);
    const label = spanH > 30 ? dt.toLocaleDateString([], { weekday: "short", hour: "2-digit" }) :
      dt.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
    g.textAlign = i === 0 ? "left" : i === nt ? "right" : "center";   // keep end labels inside the chart
    g.fillText(label, i === 0 ? P.l : i === nt ? w - P.r : X(ts), h - P.b + 8);
  }
  // markers (calibration, baseline, notes)
  (opt.markers || []).forEach((m) => {
    if (m.ts < x0 || m.ts > x1) return;
    g.save(); g.setLineDash([4, 4]); g.strokeStyle = m.kind === "calibration" ? css("--violet") : m.kind === "baseline" ? css("--green") : css("--muted");
    g.beginPath(); g.moveTo(X(m.ts), P.t); g.lineTo(X(m.ts), h - P.b); g.stroke(); g.restore();
  });
  // thresholds
  (opt.thresholds || []).forEach((th) => {
    g.save(); g.setLineDash([6, 5]); g.strokeStyle = th.color; g.lineWidth = 1.5;
    g.beginPath(); g.moveTo(P.l, Y(th.y)); g.lineTo(w - P.r, Y(th.y)); g.stroke();
    g.fillStyle = th.color; g.textAlign = "right"; g.textBaseline = "bottom"; g.fillText(th.label, w - P.r - 4, Y(th.y) - 3);
    g.restore();
  });
  const bucket = (history && history.bucket_s) || 60;
  opt.series.forEach((s) => {
    if (opt.bars) {
      const bw = Math.max(2, (X(x0 + bucket) - X(x0)) * 0.8);
      g.fillStyle = s.color;
      s.data.forEach((v, i) => { if (v) { g.fillRect(X(t[i]) - bw / 2, Y(v), bw, Y(lo) - Y(v)); } });
      return;
    }
    // area fill
    if (s.fill) {
      const grad = g.createLinearGradient(0, P.t, 0, h - P.b);
      grad.addColorStop(0, s.color + "55"); grad.addColorStop(1, s.color + "05");
      g.fillStyle = grad;
      let started = false, firstX = 0, lastX = 0;
      g.beginPath();
      s.data.forEach((v, i) => {
        if (v == null) return;
        if (!started) { g.moveTo(X(t[i]), Y(lo)); firstX = X(t[i]); started = true; }
        g.lineTo(X(t[i]), Y(v)); lastX = X(t[i]);
      });
      if (started) { g.lineTo(lastX, Y(lo)); g.lineTo(firstX, Y(lo)); g.fill(); }
    }
    g.strokeStyle = s.color; g.lineWidth = s.width || 2.2; g.lineJoin = "round";
    g.beginPath();
    let prev = null;
    s.data.forEach((v, i) => {
      if (v == null) { prev = null; return; }
      const gap = prev !== null && t[i] - t[prev] > bucket * 3;
      if (prev === null || gap) g.moveTo(X(t[i]), Y(v)); else g.lineTo(X(t[i]), Y(v));
      prev = i;
    });
    g.stroke();
  });
  // legend
  g.textAlign = "left"; g.textBaseline = "middle";
  let lx = P.l + 8;
  opt.series.forEach((s) => {
    g.fillStyle = s.color; g.fillRect(lx, P.t + 4, 14, 4);
    g.fillStyle = css("--text"); g.fillText(s.label, lx + 20, P.t + 6);
    lx += g.measureText(s.label).width + 44;
  });
  canvas._hover = { X, P, w, t, opt };
}

function attachTooltip(canvas) {
  const tip = $("tooltip");
  canvas.addEventListener("mousemove", (e) => {
    const H = canvas._hover;
    if (!H) { tip.hidden = true; return; }
    const r = canvas.getBoundingClientRect(), mx = e.clientX - r.left;
    let best = -1, bd = 1e9;
    H.t.forEach((ts, i) => { const d = Math.abs(H.X(ts) - mx); if (d < bd) { bd = d; best = i; } });
    if (best < 0 || bd > 40) { tip.hidden = true; return; }
    const lines = H.opt.series.map((s) => `<span style="color:${s.color}">●</span> ${s.label}: <b>${fmt(s.data[best], H.opt.digits ?? 1)}</b> ${H.opt.unit || ""}`);
    tip.innerHTML = `${timeStr(H.t[best])}<br>${lines.join("<br>")}`;
    tip.hidden = false;
    tip.style.left = Math.min(window.innerWidth - tip.offsetWidth - 10, e.clientX + 14) + "px";
    tip.style.top = e.clientY - 10 + "px";
  });
  canvas.addEventListener("mouseleave", () => (tip.hidden = true));
}
["chartPressure", "chartTemp", "chartWrist", "chartEvents"].forEach((id) => attachTooltip($(id)));

function drawCharts() {
  if (!history || $("tab-charts").hidden) return;
  const H = history, base = { t: H.t || [], start: H.start, end: H.end, markers: H.markers };
  const unit = (H.fsr_units && H.fsr_units[0]) || "";
  $("pressureUnit").textContent = unit === "N" ? "newtons" : "raw ADC (not calibrated)";
  drawChart($("chartPressure"), { ...base, unit, digits: unit === "N" ? 2 : 0, zero: true, series: [
    { label: "Back of hand (FSR 1)", color: css("--orange"), data: H.fsr1 || [], fill: true },
    { label: "Wrist (FSR 2)", color: css("--pink"), data: H.fsr2 || [] }] });
  const tb = analysis && analysis.temperature && analysis.temperature.baseline;
  drawChart($("chartTemp"), { ...base, unit: "°C", digits: 2, series: [
    { label: "Skin temperature", color: css("--red"), data: H.temp || [], fill: true }],
    thresholds: tb != null && state ? [{ y: tb + state.settings.temp_rise_c, label: "alert level", color: css("--warn") }] : [] });
  drawChart($("chartWrist"), { ...base, unit: "°", zero: true, series: [
    { label: "Largest movement", color: css("--violet"), data: H.max_delta || [], fill: true },
    { label: "Sideways", color: css("--blue"), data: H.deviation || [], width: 1.5 }],
    thresholds: state ? [{ y: state.settings.movement_angle_deg, label: "movement limit", color: css("--warn") }] : [] });
  drawChart($("chartEvents"), { ...base, unit: "events", digits: 0, zero: true, bars: true, series: [
    { label: "Movement events", color: css("--green"), data: H.events || [] }] });
}
window.addEventListener("resize", drawCharts);

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
      ${pts.length ? `<table class="points"><tr><th>Weight</th><th>Force</th><th>Reading</th><th></th></tr>
        ${pts.map((p) => `<tr><td>${fmt(p.force_n / 9.80665 * 1000, 0)} g</td><td>${fmt(p.force_n, 2)} N</td><td>${fmt(p.raw, 0)}</td>
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
  ["pressure_rise_min_n", "…and at least (calibrated)", "N"],
  ["pressure_rise_min_raw", "…and at least (not calibrated)", "ADC"],
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
  $("setHost").value = s.device_host || "";
  $("setBle").value = s.ble_name || "";
  $("setPatient").value = s.patient_label || "";
  LIMITS.forEach(([k]) => ($(`lim_${k}`).value = s[k]));
  setTransportUi(s.transport);
  $("baselineInfo").textContent = s.baseline_ts ? `Current baseline started ${timeStr(s.baseline_ts)}.` : "No baseline set: the start of the selected period is used.";
  settingsLoaded = true;
}

function setTransportUi(tr) {
  document.querySelectorAll("#transportSeg button").forEach((b) => b.classList.toggle("active", b.dataset.transport === tr));
  const bleMissing = state && !state.ble_available;
  $("transportHint").innerHTML = tr === "ble"
    ? "🔵 <b>Bluetooth</b>: direct connection, no router needed and lower power use. Keep the computer within about 10 m. " +
      "Set the splint to BLE mode (Serial command <b>b</b>)." +
      (bleMissing ? "<br>⚠️ The Bluetooth package is not installed. Close the app and run <b>pip install bleak</b>." : "")
    : "📶 <b>Wi-Fi</b>: the splint and this computer must be on the same network (hotspot <b>project</b>). " +
      "Longer range, but uses more battery.";
}

document.querySelectorAll("#transportSeg button").forEach((b) => b.addEventListener("click", async () => {
  try {
    await api("/api/settings", { transport: b.dataset.transport });
    setTransportUi(b.dataset.transport);
    toast(b.dataset.transport === "ble" ? "Switching to Bluetooth…" : "Switching to Wi-Fi…", "good");
  } catch (e) { toast("⚠️ " + e.message, "bad"); }
}));

$("saveSettings").addEventListener("click", async () => {
  const body = { device_host: $("setHost").value, ble_name: $("setBle").value, patient_label: $("setPatient").value };
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

$("noteBtn").addEventListener("click", async () => {
  const text = $("noteText").value.trim();
  if (!text) { toast("⚠️ Write a note first", "bad"); return; }
  await api("/api/marker", { text });
  $("noteText").value = "";
  toast("📝 Note saved", "good");
  refreshData();
});
$("noteText").addEventListener("keydown", (e) => { if (e.key === "Enter") $("noteBtn").click(); });

// ------------------------------------------------------------------ start
try {
  const r = localStorage.getItem("range");
  if (r !== null) { range = Number(r); $("range").value = r; }
  showTab(location.hash.slice(1) || localStorage.getItem("tab") || "overview");
} catch (e) { showTab(location.hash.slice(1) || "overview"); }
pollState();
refreshData();
setInterval(refreshData, 10000);
