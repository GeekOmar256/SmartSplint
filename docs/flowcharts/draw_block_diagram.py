"""Conceptual block diagram (input - process - output) of the Smart Orthopedic Splint system."""
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

OUT = Path(__file__).resolve().parent
W, H = 11.0, 5.6
fig = plt.figure(figsize=(W, H))
ax = fig.add_axes([0, 0, 1, 1])
ax.set_xlim(0, W)
ax.set_ylim(H, 0)
ax.axis("off")
FONT = "DejaVu Sans"


def box(x, y, w, h, title, lines, fc, ec, tsize=10.5, lsize=9.2):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=0.08",
                                fc=fc, ec=ec, lw=1.6, zorder=2))
    n = len(lines)
    top = y + h / 2 - (0.19 * n) / 2 - 0.08
    ax.text(x + w / 2, top, title, ha="center", va="center", fontsize=tsize, weight="bold",
            family=FONT, color="#111827", zorder=3)
    for i, line in enumerate(lines):
        ax.text(x + w / 2, top + 0.27 + 0.2 * i, line, ha="center", va="center", fontsize=lsize,
                family=FONT, color="#374151", zorder=3)


def group(x, y, w, h, label, ec):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=0.12",
                                fc="none", ec=ec, lw=1.4, ls=(0, (5, 3)), zorder=1))
    ax.text(x + 0.15, y + 0.2, label, ha="left", va="center", fontsize=10, weight="bold",
            family=FONT, color=ec)


def arrow(p, q, label=None, lpos=None, color="#1f2937", ha="center"):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle="-|>", mutation_scale=13, lw=1.5, color=color, zorder=4,
                                 shrinkA=0, shrinkB=0))
    if label:
        lx, ly = lpos if lpos else ((p[0] + q[0]) / 2, (p[1] + q[1]) / 2 - 0.13)
        ax.text(lx, ly, label, ha=ha, va="center", fontsize=8.6, family=FONT, color=color, weight="bold",
                zorder=5, bbox=dict(fc="white", ec="none", pad=0.6))


# stage bands
for x0, x1, name in ((0.25, 2.75, "INPUT"), (2.95, 8.25, "PROCESS"), (8.45, 10.75, "OUTPUT")):
    ax.add_patch(FancyBboxPatch((x0, 0.18), x1 - x0, 0.32, boxstyle="round,pad=0,rounding_size=0.06",
                                fc="#eef2ff", ec="none", zorder=1))
    ax.text((x0 + x1) / 2, 0.34, name, ha="center", va="center", fontsize=10, weight="bold",
            family=FONT, color="#3730a3")

group(0.2, 0.7, 4.95, 4.7, "On the splint", "#b45309")
group(5.45, 0.7, 5.35, 4.7, "Receiving computer", "#1d4ed8")

# inputs
box(0.45, 1.05, 2.2, 0.85, "2 × FSR 400", ["contact pressure (kPa)", "(back of hand, wrist)"], "#fef9c3", "#ca8a04")
box(0.45, 2.1, 2.2, 0.85, "DS18B20 probe", ["skin-surface", "temperature"], "#fce7f3", "#db2777")
box(0.45, 3.15, 2.2, 0.85, "2 × MPU6050 IMU", ["forearm 0x68,", "hand 0x69"], "#e0e7ff", "#4f46e5")
box(0.45, 4.3, 2.2, 0.8, "1 × 18650 cell", ["+ USB-C charge/boost"], "#fef3c7", "#b45309")

# controller
box(3.3, 1.05, 1.65, 2.95, "ESP32-C3", ["read sensors", "add sequence no.", "and time stamp", "send raw", "samples"],
    "#ede9fe", "#7c3aed")
for cy, lab in ((1.475, "ADC"), (2.525, "1-Wire"), (3.575, "I²C")):
    arrow((2.65, cy), (3.3, cy), lab, (2.975, cy - 0.15))
ax.plot([2.65, 4.125], [4.7, 4.7], color="#b45309", lw=1.5, zorder=4)
arrow((4.125, 4.7), (4.125, 4.0), color="#b45309")
ax.text(4.25, 4.42, "5 V", ha="left", va="center", fontsize=8.6, weight="bold", family=FONT, color="#b45309")
ax.text(0.45, 5.22, "The ESP32-C3 board's 3.3 V output supplies all sensors.", ha="left", va="center",
        fontsize=8.2, family=FONT, color="#92400e")

# computer application
box(5.75, 1.05, 2.4, 3.1, "SmartSplint Monitor", ["(Python application)", "", "1  receive (Bleak)",
                                                   "2  convert units,", "    apply calibration",
                                                   "3  filter, wrist angle,", "    movement events",
                                                   "4  store 1 row/s (SQLite)"], "#dbeafe", "#2563eb",
    lsize=8.8)
arrow((4.9, 2.6), (5.75, 2.6), "Bluetooth LE", (5.33, 2.4))
ax.text(5.33, 2.82, "raw samples", ha="center", va="center", fontsize=8, family=FONT, color="#374151")

# outputs
box(8.55, 1.05, 2.05, 1.55, "Web dashboard", ["live readings", "alerts and notes", "calibration"],
    "#dcfce7", "#16a34a", lsize=8.8)
arrow((8.15, 1.83), (8.55, 1.83), "HTTP", (8.35, 1.62))
box(8.55, 3.1, 2.05, 1.05, "Professional", ["review"], "#f1f5f9", "#475569")
arrow((9.58, 2.6), (9.58, 3.1))

for ext in ("png", "svg"):
    fig.savefig(OUT / f"block_diagram.{ext}", dpi=220, facecolor="white")
print("saved block_diagram")
