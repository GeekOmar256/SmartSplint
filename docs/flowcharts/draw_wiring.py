"""Wiring diagram of the Smart Orthopedic Splint (confirmed wiring), drawn with net labels.

Run: python draw_wiring.py  ->  wiring_diagram.png / .svg next to this script.
Pins with the same coloured label (e.g. 3V3, SDA) are connected together.
"""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, Rectangle

OUT = Path(__file__).resolve().parent
FONT = "Arial"
NET = {  # net name: (colour, text colour)
    "5V": ("#f97316", "white"), "3V3": ("#dc2626", "white"), "GND": ("#1f2937", "white"),
    "SDA": ("#2563eb", "white"), "SCL": ("#059669", "white"), "FSR1_OUT": ("#ca8a04", "white"),
    "FSR2_OUT": ("#9333ea", "white"), "1WIRE": ("#db2777", "white"),
    "BAT+": ("#b91c1c", "white"), "BAT−": ("#1f2937", "white"),
}
WIRE = "#334155"

fig = plt.figure(figsize=(9.2, 12.7))
ax = fig.add_axes([0, 0, 1, 1])
ax.set_xlim(0, 9.2)
ax.set_ylim(12.7, 0)
ax.axis("off")


def text(x, y, s, size=9.5, ha="center", va="center", weight="normal", color="#1f2937", style="normal"):
    ax.text(x, y, s, fontsize=size, ha=ha, va=va, family=FONT, weight=weight, color=color, style=style, zorder=6)


def tag(x, y, net, side="right"):
    """Net label attached to a wire end at (x, y)."""
    c, tc = NET[net]
    w = 0.13 + 0.085 * len(net)
    x0 = x if side == "right" else x - w
    ax.add_patch(FancyBboxPatch((x0, y - 0.13), w, 0.26, boxstyle="round,pad=0,rounding_size=0.07",
                                fc=c, ec=c, zorder=5))
    text(x0 + w / 2, y, net, size=8.5, weight="bold", color=tc)


def wire(pts, color=WIRE, lw=1.6):
    xs, ys = zip(*pts)
    ax.plot(xs, ys, color=color, lw=lw, solid_capstyle="round", zorder=3)


def dot(x, y, color=WIRE):
    ax.plot([x], [y], "o", ms=5, color=color, zorder=4)


def section(x0, y0, x1, y1, title):
    ax.add_patch(FancyBboxPatch((x0, y0), x1 - x0, y1 - y0, boxstyle="round,pad=0,rounding_size=0.12",
                                fc="#faf9ff", ec="#c4b5fd", lw=1.2, zorder=0))
    text(x0 + 0.15, y0 + 0.22, title, size=10.5, ha="left", weight="bold", color="#4c3bd6")


def module(x, y, w, title, sub, pins, side="right", fill="#eef2ff", edge="#6d5dfc", stub=0.42, pin_dy=0.33,
           top_pad=0.75):
    """Box with labelled pins on one side; pins = [(pin name, net)]. Returns the box height."""
    h = top_pad + pin_dy * len(pins) + 0.1
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=0.08",
                                fc=fill, ec=edge, lw=1.6, zorder=2))
    text(x + w / 2, y + 0.22, title, size=10, weight="bold")
    if sub:
        text(x + w / 2, y + 0.47, sub, size=8.5, color="#475569")
    for i, (pin, net) in enumerate(pins):
        py = y + top_pad + pin_dy * i + pin_dy / 2
        if side == "right":
            text(x + w - 0.1, py, pin, size=8.8, ha="right", weight="bold")
            wire([(x + w, py), (x + w + stub, py)], NET[net][0])
            tag(x + w + stub, py, net, "right")
        else:
            text(x + 0.1, py, pin, size=8.8, ha="left", weight="bold")
            wire([(x - stub, py), (x, py)], NET[net][0])
            tag(x - stub, py, net, "left")
    return h


def resistor_v(x, y_top, y_bot, label, color=WIRE, box_h=0.42):
    """Vertical resistor between y_top and y_bot (wire + body)."""
    ym = (y_top + y_bot) / 2
    wire([(x, y_top), (x, ym - box_h / 2)], color)
    wire([(x, ym + box_h / 2), (x, y_bot)], color)
    ax.add_patch(Rectangle((x - 0.09, ym - box_h / 2), 0.18, box_h, fc="white", ec=WIRE, lw=1.5, zorder=4))
    text(x + 0.17, ym, label, size=8.8, ha="left", weight="bold")


# ------------------------------------------------------------------ title
text(0.3, 0.32, "Smart Orthopedic Splint – Wiring Diagram", size=14, ha="left", weight="bold", color="#1f2937")
text(0.3, 0.62, "ESP32-C3 SuperMini · 2 × MPU6050 · 2 × FSR 400 · DS18B20 · 2 × 18650 + Type-C charge/boost module",
     size=9, ha="left", color="#475569")

# ------------------------------------------------------------------ power supply
section(0.25, 0.9, 8.95, 3.05, "Power supply")
# two cells in parallel
bx, by = 0.6, 1.4
for i in range(2):
    ax.add_patch(FancyBboxPatch((bx, by + 0.55 * i), 1.6, 0.4, boxstyle="round,pad=0,rounding_size=0.18",
                                fc="#fef3c7", ec="#b45309", lw=1.5, zorder=2))
    text(bx + 0.8, by + 0.2 + 0.55 * i, f"18650 cell {i + 1}", size=8.8, weight="bold")
    text(bx + 0.12, by + 0.2 + 0.55 * i, "−", size=11, weight="bold")
    text(bx + 1.48, by + 0.2 + 0.55 * i, "+", size=11, weight="bold")
text(bx + 0.8, by + 1.22, "3.7 V, connected in parallel", size=8.5, color="#475569")
xp, xn = bx + 1.95, bx - 0.15                       # + and − rails of the pack
for i in range(2):
    yc = by + 0.2 + 0.55 * i
    wire([(bx + 1.6, yc), (xp, yc)], NET["BAT+"][0])
    wire([(xn, yc), (bx, yc)], NET["BAT−"][0])
wire([(xp, by + 0.2), (xp, by + 0.75)], NET["BAT+"][0])
wire([(xn, by + 0.2), (xn, by + 0.75)], NET["BAT−"][0])
dot(xp, by + 0.2, NET["BAT+"][0]); dot(xp, by + 0.75, NET["BAT+"][0])
dot(xn, by + 0.2, NET["BAT−"][0]); dot(xn, by + 0.75, NET["BAT−"][0])

# charge / boost module
mx, my, mw = 3.55, 1.25, 2.2
mh = 1.55
ax.add_patch(FancyBboxPatch((mx, my), mw, mh, boxstyle="round,pad=0,rounding_size=0.08",
                            fc="#ecfdf5", ec="#16a34a", lw=1.6, zorder=2))
text(mx + mw / 2, my + 0.22, "Type-C charge / boost module", size=9.5, weight="bold")
text(mx + mw / 2, my + 0.47, "USB Type-C input for charging", size=8.3, color="#475569")
pins_l = [("B+", my + 0.85, "BAT+"), ("B−", my + 1.25, "BAT−")]
pins_r = [("OUT+ 5 V", my + 0.85, "5V"), ("OUT−", my + 1.25, "GND")]
for name, y, net in pins_l:
    text(mx + 0.1, y, name, size=8.8, ha="left", weight="bold")
for name, y, net in pins_r:
    text(mx + mw - 0.1, y, name, size=8.8, ha="right", weight="bold")
# battery rails to module
wire([(xp, by + 0.475), (xp + 0.35, by + 0.475), (xp + 0.35, my + 0.85), (mx, my + 0.85)], NET["BAT+"][0])
dot(xp, by + 0.475, NET["BAT+"][0])
wire([(xn, by + 0.75), (xn, by + 1.55), (xp + 0.55, by + 1.55), (xp + 0.55, my + 1.25), (mx, my + 1.25)],
     NET["BAT−"][0])
# module outputs -> net labels
for name, y, net in pins_r:
    wire([(mx + mw, y), (mx + mw + 0.42, y)], NET[net][0])
    tag(mx + mw + 0.42, y, net)
text(7.0, 1.55, "Boosts 3.0–4.2 V\nto a stable 5 V\nfor the ESP32-C3", size=8.5, ha="left", color="#475569")

# ------------------------------------------------------------------ microcontroller
section(0.25, 3.25, 8.95, 6.38, "Microcontroller")
ex, ey, ew = 3.55, 3.6, 2.2
pins_left = [("5V", "5V"), ("GND", "GND"), ("3V3", "3V3")]
pins_right = [("GPIO0 (A0)", "FSR1_OUT"), ("GPIO1 (A1)", "FSR2_OUT"), ("GPIO3", "1WIRE"),
              ("GPIO8 (SDA)", "SDA"), ("GPIO9 (SCL)", "SCL")]
eh = 0.75 + 0.36 * len(pins_right) + 0.15
ax.add_patch(FancyBboxPatch((ex, ey), ew, eh, boxstyle="round,pad=0,rounding_size=0.08",
                            fc="#eef2ff", ec="#6d5dfc", lw=1.8, zorder=2))
text(ex + ew / 2, ey + 0.22, "ESP32-C3 SuperMini", size=10.5, weight="bold")
text(ex + ew / 2, ey + 0.47, "Wi-Fi + Bluetooth LE", size=8.5, color="#475569")
for i, (pin, net) in enumerate(pins_left):
    py = ey + 0.95 + 0.36 * i
    text(ex + 0.1, py, pin, size=8.8, ha="left", weight="bold")
    wire([(ex - 0.42, py), (ex, py)], NET[net][0])
    tag(ex - 0.42, py, net, "left")
for i, (pin, net) in enumerate(pins_right):
    py = ey + 0.95 + 0.36 * i
    text(ex + ew - 0.1, py, pin, size=8.8, ha="right", weight="bold")
    wire([(ex + ew, py), (ex + ew + 0.42, py)], NET[net][0])
    tag(ex + ew + 0.42, py, net)
text(0.55, 4.55, "Input: 5 V from the\nboost module.\nOutput: 3.3 V from the\nboard's regulator\npowers all sensors.",
     size=8.3, ha="left", color="#475569")
text(7.55, 4.3, "Analog inputs\n(12-bit ADC)", size=8.3, ha="left", color="#475569")
text(7.55, 5.4, "1-Wire and\nI2C buses", size=8.3, ha="left", color="#475569")

# ------------------------------------------------------------------ sensors
section(0.25, 6.45, 8.95, 12.0, "Sensors")
R2 = 9.75   # second sensor row
# IMUs
h1 = module(0.55, 6.85, 1.95, "GY-521 MPU6050", "Forearm IMU · address 0x68",
            [("VCC", "3V3"), ("GND", "GND"), ("SCL", "SCL"), ("SDA", "SDA"), ("AD0", "GND")])
module(4.5, 6.85, 1.95, "GY-521 MPU6050", "Hand IMU · address 0x69",
       [("VCC", "3V3"), ("GND", "GND"), ("SCL", "SCL"), ("SDA", "SDA"), ("AD0", "3V3")])

# FSR dividers
for k, (x, net) in enumerate([(0.95, "FSR1_OUT"), (2.55, "FSR2_OUT")]):
    yt = R2 + 0.75
    yn, yb = yt + 0.6, yt + 1.17
    c3 = NET["3V3"][0]
    ax.add_patch(FancyBboxPatch((x - 0.27, yt - 0.42), 0.54, 0.26, boxstyle="round,pad=0,rounding_size=0.07",
                                fc=c3, ec=c3, zorder=5))
    text(x, yt - 0.29, "3V3", size=8.5, weight="bold", color="white")
    resistor_v(x, yt - 0.16, yn, f"FSR {k + 1}")
    ax.add_patch(FancyBboxPatch((x - 0.13, (yt - 0.16 + yn) / 2 - 0.21), 0.26, 0.42,
                                boxstyle="round,pad=0,rounding_size=0.04", fc="#fde047", ec="#a16207", lw=1.5,
                                zorder=4))
    dot(x, yn)
    wire([(x, yn), (x + 0.35, yn)], NET[net][0])
    tag(x + 0.35, yn, net)
    resistor_v(x, yn, yb, "10 kΩ")
    cg = NET["GND"][0]
    ax.add_patch(FancyBboxPatch((x - 0.25, yb), 0.5, 0.24, boxstyle="round,pad=0,rounding_size=0.07",
                                fc=cg, ec=cg, zorder=5))
    text(x, yb + 0.12, "GND", size=8.5, weight="bold", color="white")
text(0.55, R2 + 0.05, "FSR voltage dividers (back of hand, wrist)", size=8.8, ha="left", weight="bold", color="#334155")

# DS18B20 + pull-up
module(4.5, R2 + 0.15, 1.95, "DS18B20 probe", "Waterproof, 1-Wire",
       [("VDD (red)", "3V3"), ("DQ (yellow)", "1WIRE"), ("GND (black)", "GND")], fill="#fdf2f8", edge="#db2777")
px = 8.25
c3 = NET["3V3"][0]
ax.add_patch(FancyBboxPatch((px - 0.27, R2 + 0.2), 0.54, 0.26, boxstyle="round,pad=0,rounding_size=0.07",
                            fc=c3, ec=c3, zorder=5))
text(px, R2 + 0.33, "3V3", size=8.5, weight="bold", color="white")
resistor_v(px, R2 + 0.46, R2 + 1.35, "")
text(px - 0.17, R2 + 0.9, "4.7 kΩ\npull-up", size=8.5, ha="right", weight="bold")
c1 = NET["1WIRE"][0]
ax.add_patch(FancyBboxPatch((px - 0.33, R2 + 1.35), 0.66, 0.26, boxstyle="round,pad=0,rounding_size=0.07",
                            fc=c1, ec=c1, zorder=5))
text(px, R2 + 1.48, "1WIRE", size=8.5, weight="bold", color="white")

# ------------------------------------------------------------------ legend
text(0.3, 12.25, "Labels with the same name and colour are connected together (e.g. every 3V3 label is the 3.3 V supply).",
     size=8.8, ha="left", color="#334155")
text(0.3, 12.5, "Both IMUs share the I2C bus; the AD0 pin selects the address (GND → 0x68, 3V3 → 0x69). "
                 "Wire colours of the DS18B20 probe can differ: check the probe label.", size=8.8, ha="left",
     color="#334155")

for ext in ("png", "svg"):
    fig.savefig(OUT / f"wiring_diagram.{ext}", dpi=220, facecolor="white")
print("saved wiring_diagram")
