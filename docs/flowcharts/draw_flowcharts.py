"""Draw the SmartSplint flowcharts with straight, right-angled connectors.

Run:  python draw_flowcharts.py
Writes six PNG + SVG files next to this script (sized to stay readable on an A4 page):
    1a_esp32_setup, 1b_esp32_main_loop, 2a_app_startup_dashboard, 2b_app_data_link,
    3a_processing_convert_filter, 3b_processing_wrist_events_storage

Layout rules: decisions sit on a vertical main line, "Yes" branches go to the right and rejoin the
main line below (junction dot), so every connector is made of horizontal and vertical segments.
Box sizes are measured from the text, so labels never wrap. Units are inches.
"""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.font_manager import FontProperties
from matplotlib.patches import FancyBboxPatch, Polygon
from matplotlib.textpath import TextPath

OUT = Path(__file__).resolve().parent
FONT = "Arial"
FS = 10.5
LINE = "#334155"
LABEL = "#15803d"
STYLE = {                      # fill, border, text
    "term": ("#6d5dfc", "#4c3bd6", "#ffffff"),
    "act": ("#eef2ff", "#6d5dfc", "#1f2937"),
    "dec": ("#fff4e5", "#f97316", "#1f2937"),
    "out": ("#dcfce7", "#16a34a", "#1f2937"),
    "warn": ("#fde2e4", "#ec4899", "#1f2937"),
}
PROP = FontProperties(family=FONT, size=FS)
LINE_H = FS * 1.32 / 72
GAP = 0.42


def text_size(text):
    lines = text.split("\n")
    w = max(TextPath((0, 0), ln or " ", size=FS, prop=PROP).get_extents().width for ln in lines) / 72
    return w, len(lines) * LINE_H


class Node:
    def __init__(self, kind, text, x=0.0, y=0.0):
        self.kind, self.text, self.x, self.y = kind, text, x, y
        tw, th = text_size(text)
        if kind == "dec":
            self.w, self.h = tw * 1.55 + 0.45, th * 1.75 + 0.42
        elif kind == "term":
            self.w, self.h = tw + 0.62, th + 0.26
        else:
            self.w, self.h = tw + 0.40, th + 0.30

    top = property(lambda s: (s.x, s.y - s.h / 2))
    bottom = property(lambda s: (s.x, s.y + s.h / 2))
    left = property(lambda s: (s.x - s.w / 2, s.y))
    right = property(lambda s: (s.x + s.w / 2, s.y))


class Chart:
    def __init__(self):
        self.nodes, self.paths, self.panels, self.dots = [], [], [], []

    def add(self, kind, text, x, y):
        n = Node(kind, text, x, y)
        self.nodes.append(n)
        return n

    def stack(self, x, y_top, items):
        out, y = [], y_top
        for kind, text in items:
            n = Node(kind, text, x, 0)
            n.y = y + n.h / 2
            y = n.y + n.h / 2 + GAP
            self.nodes.append(n)
            out.append(n)
        return out

    def path(self, pts, label=None, arrow=True, side="left"):
        self.paths.append(dict(pts=pts, label=label, arrow=arrow, side=side))

    def down(self, a, b, label=None):
        (x1, y1), (x2, y2) = a.bottom, b.top
        if abs(x1 - x2) < 1e-6:
            self.path([(x1, y1), (x2, y2)], label)
        else:
            ym = (y1 + y2) / 2
            self.path([(x1, y1), (x1, ym), (x2, ym), (x2, y2)], label)

    def chain(self, nodes, labels=None):
        for i, (a, b) in enumerate(zip(nodes, nodes[1:])):
            self.down(a, b, (labels or {}).get(i))

    def branch(self, dec, box, label="Yes"):
        self.path([dec.right, box.left], label, side="above")

    def rejoin(self, box, target, spine_x):
        x, y = box.bottom
        yj = target.top[1] - GAP * 0.45
        self.path([(x, y), (x, yj), (spine_x, yj)], arrow=False)
        self.dots.append((spine_x, yj))
        return yj

    def panel(self, x0, y0, x1, y1, title):
        self.panels.append((x0, y0, x1, y1, title))

    def draw(self, name, margin=0.2):
        xs, ys = [], []
        for n in self.nodes:
            xs += [n.x - n.w / 2, n.x + n.w / 2]
            ys += [n.y - n.h / 2, n.y + n.h / 2]
        for x0, y0, x1, y1, _ in self.panels:
            xs += [x0, x1]
            ys += [y0, y1]
        for p in self.paths:
            xs += [q[0] for q in p["pts"]]
            ys += [q[1] for q in p["pts"]]
        X0, X1, Y0, Y1 = min(xs) - margin, max(xs) + margin, min(ys) - margin, max(ys) + margin
        W, H = X1 - X0, Y1 - Y0
        fig = plt.figure(figsize=(W, H))
        ax = fig.add_axes([0, 0, 1, 1])
        ax.set_xlim(X0, X1)
        ax.set_ylim(Y1, Y0)
        ax.axis("off")

        for x0, y0, x1, y1, title in self.panels:
            ax.add_patch(FancyBboxPatch((x0, y0), x1 - x0, y1 - y0, boxstyle="round,pad=0,rounding_size=0.12",
                                        fc="#faf9ff", ec="#c4b5fd", lw=1.2, zorder=0))
            ax.text(x1 - 0.18, y1 - 0.2, title, ha="right", va="center", fontsize=FS + 0.5, family=FONT,
                    weight="bold", color="#4c3bd6", zorder=7)

        for p in self.paths:
            pts = p["pts"]
            xs_, ys_ = zip(*pts)
            ax.plot(xs_, ys_, color=LINE, lw=1.4, solid_capstyle="butt", zorder=2)
            if p["arrow"]:
                ax.annotate("", xy=pts[-1], xytext=pts[-2],
                            arrowprops=dict(arrowstyle="-|>,head_length=0.55,head_width=0.28", color=LINE,
                                            lw=1.4, shrinkA=0, shrinkB=0), zorder=3)
            if p["label"]:
                (x_, y_), (bx, by) = pts[0], pts[1]
                if p["side"] == "above":            # horizontal segment: just above, near the start
                    lx, ly, ha, va = x_ + 0.1, y_ - 0.05, "left", "bottom"
                elif p["side"] == "right":          # vertical segment: to the right, near the start
                    lx, ly, ha, va = x_ + 0.07, y_ + min(0.15, abs(by - y_) / 2), "left", "center"
                else:                               # vertical segment: to the left, near the start
                    lx, ly, ha, va = x_ - 0.07, y_ + min(0.17, abs(by - y_) / 2), "right", "center"
                ax.text(lx, ly, p["label"], ha=ha, va=va, fontsize=FS - 0.5, family=FONT, color=LABEL,
                        weight="bold", zorder=4)
        for x, y in self.dots:
            ax.plot([x], [y], "o", ms=4.2, color=LINE, zorder=3)

        for n in self.nodes:
            fc, ec, tc = STYLE[n.kind]
            if n.kind == "dec":
                ax.add_patch(Polygon([n.top, n.right, n.bottom, n.left], closed=True, fc=fc, ec=ec, lw=1.5, zorder=5))
            else:
                r = n.h / 2 if n.kind == "term" else 0.06
                ax.add_patch(FancyBboxPatch((n.x - n.w / 2, n.y - n.h / 2), n.w, n.h,
                                            boxstyle=f"round,pad=0,rounding_size={r}", fc=fc, ec=ec, lw=1.5, zorder=5))
            ax.text(n.x, n.y, n.text, ha="center", va="center", fontsize=FS, family=FONT, color=tc,
                    linespacing=1.32, zorder=6, weight="bold" if n.kind == "term" else "normal")

        for ext in ("png", "svg"):
            fig.savefig(OUT / f"{name}.{ext}", dpi=220, facecolor="white")
        plt.close(fig)
        print(f"{name}: {W:.1f} x {H:.1f} in")


def side_branches(c, spine, items, x_side, spine_x):
    """items: (decision index, kind, text, rejoin?) -> boxes to the right of the decisions."""
    for i, kind, text, back in items:
        box = c.add(kind, text, x_side, spine[i].y)
        c.branch(spine[i], box)
        if back:
            c.rejoin(box, spine[i + 1], spine_x)


# ===================================================================== 1a. ESP32 setup
def esp32_setup():
    c = Chart()
    sx = 1.95
    s = c.stack(sx, 0.2, [
        ("term", "Power on"),
        ("act", "Start Serial (115200)\nStart I2C on GPIO8/GPIO9 at 100 kHz\nSet FSR ADC on GPIO0/GPIO1 (12-bit)"),
        ("act", "Scan the I2C bus\nConfigure IMUs at 0x68 and 0x69\n(±2 g, ±250 °/s)"),
        ("act", "Find the DS18B20 on GPIO3\nStart the first conversion"),
        ("act", "Start the BLE server\nAdvertise as SmartSplint"),
        ("term", "Go to the main loop"),
    ])
    c.chain(s)
    c.draw("1a_esp32_setup")


# ===================================================================== 1b. ESP32 main loop
def esp32_main_loop():
    c = Chart()
    lx = 1.55
    L = c.stack(lx, 0.2, [
        ("term", "Loop start"),
        ("dec", "BLE connected\nand samples\nready to send?"),
        ("dec", "Serial\ncommand r / s?"),
        ("dec", "20 ms since\nlast sample?"),
        ("dec", "5 s since last\nstatus check?"),
        ("dec", "500 ms since\nlast IMU print?"),
        ("dec", "Probe found and\nconversion due?"),
        ("term", "Back to loop start"),
    ])
    c.chain(L, {i: "No" for i in range(1, 7)})
    side_branches(c, L, [
        (1, "act", "Pack up to 5 samples per packet\nSend a BLE notification", True),
        (2, "act", "Reinitialize sensors or scan I2C", True),
        (3, "act", "Read both IMUs and both FSRs\nStore sample in ring buffer (512)", True),
        (4, "act", "Retry any stopped IMU\nPrint sensor and BLE status", True),
        (5, "act", "Print raw IMU diagnostics", True),
        (6, "act", "Read DS18B20 (°C)\nStart the next conversion", True),
    ], 4.85, lx)
    c.draw("1b_esp32_main_loop")


# ===================================================================== 2a. App startup + dashboard
def app_startup_dashboard():
    c = Chart()
    mx = 3.75
    top = c.stack(mx, 0.2, [
        ("term", "Run server.py (run.bat)"),
        ("act", "Open the SQLite database\nLoad settings.json and calibration.json"),
        ("act", "Start the data-link thread"),
        ("act", "Start the web server on localhost:8000\nand open Measurements"),
    ])
    c.chain(top)
    thread = c.add("term", "Data-link thread\nruns in parallel", 7.05, top[2].y)
    c.path([top[2].right, thread.left])

    py = top[-1].y + top[-1].h / 2 + 0.55
    cy = py + 0.3
    x1, x2, x3 = 1.0, 3.75, 6.3
    col1 = c.stack(x1, cy, [("act", "Every 1 s:\nread\n/api/state"),
                            ("out", "Measurements,\nraw readings if\nuncalibrated and\nconnection status")])
    col2 = c.stack(x2, cy, [("act", "Every 10 s: read\n/api/history and\n/api/analysis"),
                            ("act", "Read the rows of\nthe selected period"),
                            ("act", "Analysis:\npressure and temperature\nvs baseline\nmovement events and\ncompliance\nloose-splint check\nmovement–pressure link\ndata quality"),
                            ("out", "Alerts, status and\ncompliance on\nMeasurements")])
    col3 = c.stack(x3, cy, [("act", "Gear icon > Settings\nSelect Calibration"),
                            ("dec", "Password\naccepted?"),
                            ("act", "Open Calibration\nCapture and check data"),
                            ("dec", "Valid?"),
                            ("out", "Save calibration\nfor later sessions")])
    c.chain(col1)
    c.chain(col2)
    c.chain(col3, {1: "Yes", 3: "Yes"})
    bad_password = c.add("warn", "Keep page\nlocked", 8.1, col3[1].y)
    c.branch(col3[1], bad_password, "No")
    bad = c.add("warn", "Show why\ncapture failed", 8.1, col3[3].y)
    c.branch(col3[3], bad, "No")
    web = top[-1]
    yb = py - 0.25
    c.path([web.bottom, (web.x, yb)], arrow=False)
    c.path([(x1, yb), (x3, yb)], arrow=False)
    for n in (col1[0], col2[0], col3[0]):
        c.path([(n.x, yb), n.top])
    bottom = max(col2[-1].y + col2[-1].h / 2, col3[-1].y + col3[-1].h / 2) + 0.5
    c.panel(0.15, py, 9.0, bottom, "Dashboard in the browser")
    c.draw("2a_app_startup_dashboard")


# ===================================================================== 2b. App data-link thread
def app_data_link():
    c = Chart()
    lx = 3.75
    start = c.stack(lx, 0.2, [("term", "Data-link thread\n(repeats every 0.5 s)"),
                              ("act", "Find 'SmartSplint',\nconnect, receive and\ndecode BLE packets"),
                              ("dec", "Data\nreceived?")])
    c.chain(start)
    got = start[-1]
    rest = c.stack(lx, got.y + got.h / 2 + GAP, [
        ("act", "Process each raw sample\n(see the processing flowchart)"),
        ("act", "Store one row per second\nin the database"),
        ("act", "Update the live values\nand the connection status"),
        ("term", "Next cycle"),
    ])
    c.down(got, rest[0], "Yes")
    c.chain(rest)
    retry = c.add("warn", "Show the reason\n(not found, lost,\nno reply)\nand retry", 6.15, got.y)
    c.branch(got, retry, "No")
    nxt = rest[-1]
    c.path([retry.bottom, (retry.x, nxt.y), nxt.right])
    c.draw("2b_app_data_link")


# ===================================================================== 3a. Processing step 1
def processing_step1():
    c = Chart()
    ax = 2.3
    A = c.stack(ax, 0.2, [
        ("term", "Raw sample from the splint\n(50 per second)"),
        ("act", "Check the sample number\nand count missed samples"),
        ("act", "Convert to units:\naccel ÷ 16384 → g, gyro ÷ 131 → °/s\nSubtract the saved gyro offsets"),
        ("dec", "Calibration\ncapture running?"),
        ("act", "Tilt from gravity for each IMU:\nθacc = atan2(−ax, √(ay² + az²))"),
        ("act", "Complementary filter for each IMU:\nθ = 0.98·(θ + ωy·Δt) + 0.02·θacc"),
        ("term", "Continue to step 2"),
    ])
    c.chain(A, {3: "No"})
    side_branches(c, A, [(3, "act", "Add the sample to\nthe calibration sums", True)], 5.45, ax)
    c.draw("3a_processing_convert_filter")


# ===================================================================== 3b. Processing step 2
def processing_step2():
    c = Chart()
    bx = 2.2
    B = c.stack(bx, 0.2, [
        ("term", "From step 1"),
        ("dec", "Both IMUs OK\nand IMU calibration\nsaved?"),
        ("act", "Wrist movement:\nΔθ = (θhand − θforearm) − θ0\nRelative rotation:\nωrel = ωz,hand − ωz,forearm"),
        ("act", "Sideways movement:\nintegrate ωrel,\nreset after 1 s at rest"),
        ("dec", "|Δθ| > 5° or\nsideways > 8°?"),
        ("act", "Convert FSR1 and FSR2 to newtons\n(piecewise-linear calibration)"),
        ("dec", "New second\nstarted?"),
        ("act", "Update the live values\nfor the dashboard"),
        ("term", "Next sample"),
    ])
    c.chain(B[:2])
    c.down(B[1], B[2], "Yes")
    c.chain(B[2:5])
    c.down(B[4], B[5], "No")
    c.chain(B[5:7])
    c.down(B[6], B[7], "No")
    c.chain(B[7:])
    ev = c.add("out", "Count a\nmovement event", 5.2, B[4].y)
    c.branch(B[4], ev, "Yes, new")
    yj = c.rejoin(ev, B[5], bx)
    row = c.add("out", "Close the previous second:\naverages, maxima, events\n→ database row", 5.2, B[6].y)
    c.branch(B[6], row)
    c.rejoin(row, B[7], bx)
    # "No": skip the wrist calculation and go straight to the FSR conversion
    xb = 6.85
    c.path([B[1].right, (xb, B[1].y), (xb, yj), (ev.x, yj)], "No", arrow=False, side="above")
    c.draw("3b_processing_wrist_events_storage")


if __name__ == "__main__":
    for old in OUT.glob("[123]_*.png"):      # remove the previous combined charts
        old.unlink()
    for old in OUT.glob("[123]_*.svg"):
        old.unlink()
    esp32_setup()
    esp32_main_loop()
    app_startup_dashboard()
    app_data_link()
    processing_step1()
    processing_step2()
