"""Generate docs/wiring-diagram.svg (and optionally a high-res PNG).

Usage:
    python docs/make_wiring_diagram.py            # writes the SVG
    python docs/make_wiring_diagram.py --png      # also renders the PNG via
                                                  # headless Chrome/Edge (3x)

Conventions used in the drawing:
  * A filled dot joins two wires.
  * A small hop (half-circle) means the wires cross without connecting.
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SVG_PATH = os.path.join(HERE, "wiring-diagram.svg")
PNG_PATH = os.path.join(HERE, "wiring-diagram.png")

W, H = 1600, 1180
SCALE = 3  # PNG is W*SCALE x H*SCALE pixels

RED = "#d62728"
BLK = "#222222"
LED = "#ff7f0e"   # LED data
RAD = "#1f77b4"   # radar data
FONT = "Segoe UI, Helvetica, Arial, sans-serif"

BUS_5V = 330
BUS_GND = 360
HOP = 8

out = []


def add(s):
    out.append(s)


def text(x, y, s, size=14, anchor="start", weight="normal", fill="#222", italic=False):
    style = ' font-style="italic"' if italic else ""
    s = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    add(f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" '
        f'font-weight="{weight}" fill="{fill}"{style}>{s}</text>')


def dot(x, y, color):
    add(f'<circle cx="{x}" cy="{y}" r="5" fill="{color}"/>')


def vwire(x, y1, y2, color, width=3, hops=(), dash=False):
    """Vertical wire from y1 to y2 with hops at the given y values."""
    lo, hi = sorted((y1, y2))
    d = f"M{x},{y1}"
    pts = sorted(hops, reverse=(y1 > y2))
    for hy in pts:
        if y1 < y2:
            d += f" L{x},{hy - HOP} A{HOP},{HOP} 0 0 1 {x},{hy + HOP}"
        else:
            d += f" L{x},{hy + HOP} A{HOP},{HOP} 0 0 0 {x},{hy - HOP}"
    d += f" L{x},{y2}"
    da = ' stroke-dasharray="10 6"' if dash else ""
    add(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{width}"{da}/>')


def path(d, color, width=3, dash=False):
    da = ' stroke-dasharray="10 6"' if dash else ""
    add(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{width}" '
        f'stroke-linejoin="round"{da}/>')


def resistor_v(x, ytop, label):
    """Vertical resistor body (box style) from ytop to ytop+44."""
    add(f'<rect x="{x - 9}" y="{ytop}" width="18" height="44" rx="3" '
        f'fill="#f3e4c8" stroke="#555" stroke-width="2"/>')
    text(x - 16, ytop + 27, label, 13, anchor="end", weight="bold")


def cap_h(x1, x2, y, label):
    """Electrolytic capacitor between x1 (+) and x2 (-) on a horizontal line."""
    mid = (x1 + x2) / 2
    path(f"M{x1},{y} L{mid - 5},{y}", RED)
    path(f"M{mid + 5},{y} L{x2},{y}", BLK)
    add(f'<line x1="{mid - 5}" y1="{y - 12}" x2="{mid - 5}" y2="{y + 12}" stroke="#333" stroke-width="3"/>')
    add(f'<path d="M{mid + 9},{y - 12} Q{mid + 3},{y} {mid + 9},{y + 12}" fill="none" stroke="#333" stroke-width="3"/>')
    text(mid - 13, y - 6, "+", 13, anchor="middle", weight="bold", fill=RED)
    text(x1 - 10, y + 5, label, 12, anchor="end")
    dot(x1, y, RED)
    dot(x2, y, BLK)


def pin_label_box(x, y, s, fill="#ffffff", color="#222"):
    w = 8 * len(s) + 10
    add(f'<rect x="{x - w / 2}" y="{y - 13}" width="{w}" height="18" rx="3" fill="{fill}" opacity="0.92"/>')
    text(x, y, s, 12, anchor="middle", weight="bold", fill=color)


def led_matrix(x, y, title, pins):
    w, h = 220, 150
    text(x + w / 2, y - 10, title, 16, anchor="middle", weight="bold")
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" fill="#151515" stroke="#000" stroke-width="2"/>')
    add(f'<rect x="{x + 10}" y="{y + 8}" width="{w - 20}" height="{h - 48}" fill="url(#ledgrid)"/>')
    text(x + w / 2, y + h - 26, "16×16 WS2812B · IN end", 12, anchor="middle", fill="#ddd")
    for px, lab in pins:
        add(f'<rect x="{px - 6}" y="{y + h - 4}" width="12" height="10" fill="#c9a227" stroke="#7a6212"/>')
        text(px, y + h - 9, lab, 11, anchor="middle", weight="bold", fill="#fff")


def radar(x, y, title, subtitle):
    w, h = 240, 170
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" fill="#1e6b3a" stroke="#0e3d20" stroke-width="2"/>')
    # antenna patches
    for i in range(4):
        add(f'<rect x="{x + 70 + i * 26}" y="{y + 92}" width="16" height="16" fill="#d4af37" opacity="0.85"/>')
    text(x + w / 2, y + 50, title, 18, anchor="middle", weight="bold", fill="#fff")
    text(x + w / 2, y + 72, "HLK-LD2451 24 GHz radar", 13, anchor="middle", fill="#e0f2e7")
    text(x + w / 2, y + 140, subtitle, 12, anchor="middle", fill="#cfe9d8", italic=True)
    text(x + w / 2, y + 158, "5 V in · 3.3 V UART · ≥ 300 mA", 11, anchor="middle", fill="#cfe9d8")


def divider_box(cx, cy):
    add(f'<rect x="{cx - 42}" y="{cy - 15}" width="84" height="30" rx="4" fill="#fff" stroke="{RAD}" stroke-width="2"/>')
    text(cx, cy + 5, "5 V → 3.3 V", 12, anchor="middle", weight="bold", fill=RAD)
    text(cx, cy + 34, "1 kΩ / 2 kΩ divider", 11, anchor="middle", fill="#444")
    text(cx, cy + 49, "or level shifter", 11, anchor="middle", fill="#444")


# ---------------------------------------------------------------- document
add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
    f'viewBox="0 0 {W} {H}" font-family="{FONT}">')
add('<defs><pattern id="ledgrid" width="12.5" height="6.4" patternUnits="userSpaceOnUse">'
    '<circle cx="6.25" cy="3.2" r="2" fill="#3a3a3a"/></pattern></defs>')
add(f'<rect width="{W}" height="{H}" fill="#ffffff"/>')

text(40, 48, "radar-sign — wiring diagram", 28, weight="bold")
text(40, 74, "Arduino Mega 2560 · 2× HLK-LD2451 radar · 2× 16×16 WS2812B matrix · "
             "powered from a 5 V USB charger", 15, fill="#555")

# ---------------------------------------------------------------- USB power
add('<rect x="40" y="110" width="170" height="100" rx="10" fill="#eeeeee" stroke="#333" stroke-width="2"/>')
add('<rect x="22" y="138" width="18" height="8" fill="#999"/><rect x="22" y="174" width="18" height="8" fill="#999"/>')
text(125, 148, "USB charger", 16, anchor="middle", weight="bold")
text(125, 170, "5 V · 3 A or more", 13, anchor="middle")
text(125, 190, "(USB-A or USB-C)", 12, anchor="middle", fill="#555")
add('<rect x="210" y="148" width="12" height="24" fill="#888" stroke="#444"/>')
add('<line x1="222" y1="160" x2="330" y2="160" stroke="#666" stroke-width="14"/>')
add('<rect x="326" y="151" width="6" height="18" fill="#444"/>')
text(276, 140, "USB cable", 12, anchor="middle", weight="bold")
text(276, 190, "device end cut off", 11, anchor="middle", fill="#555")

# fanned-out conductors
path("M332,155 C352,155 352,120 375,120 L560,120", RED, 4)
path("M332,158 C352,158 352,140 375,140 L720,140", BLK, 4)
for yy, lab in ((180, "D− (white) — not used: insulate"), (200, "D+ (green) — not used: insulate")):
    start = 162 if yy == 180 else 165
    d = f"M332,{start} C352,{start} 352,{yy} 375,{yy} L410,{yy}"
    path(d, "#888", 5)
    if yy == 180:
        path(d, "#ffffff", 3)
    else:
        path(d, "#2ca02c", 3)
    add(f'<rect x="410" y="{yy - 6}" width="22" height="12" rx="2" fill="#555"/>')
    text(442, yy + 4, lab, 12, fill="#555")
text(385, 113, "VBUS +5 V (red)", 12, weight="bold", fill=RED)
text(385, 134, "GND (black)", 12, weight="bold", fill=BLK)

# fuse
add('<rect x="560" y="111" width="64" height="18" rx="9" fill="#fff" stroke="#333" stroke-width="2"/>')
add('<line x1="566" y1="120" x2="618" y2="120" stroke="#999" stroke-width="2"/>')
text(592, 103, "3 A fuse", 12, anchor="middle", weight="bold")
path(f"M624,120 L760,120 L760,{BUS_5V}", RED, 4)
dot(760, BUS_5V, RED)
vwire(720, 140, BUS_GND, BLK, 4, hops=[BUS_5V])
dot(720, BUS_GND, BLK)

# ---------------------------------------------------------------- buses
add(f'<line x1="60" y1="{BUS_5V}" x2="1560" y2="{BUS_5V}" stroke="{RED}" stroke-width="7" stroke-linecap="round"/>')
add(f'<line x1="60" y1="{BUS_GND}" x2="1560" y2="{BUS_GND}" stroke="{BLK}" stroke-width="7" stroke-linecap="round"/>')
text(60, BUS_5V - 10, "+5 V bus", 13, weight="bold", fill=RED)
text(60, BUS_GND + 22, "GND bus", 13, weight="bold", fill=BLK)
text(200, BUS_5V - 10, "terminal strip / lever connectors · 18 AWG to the matrices",
     12, fill="#555")

# ---------------------------------------------------------------- LED matrices
def matrix_block(x, title, din_to_x, din_to_y, jog_y):
    p5, pg, pd = x + 50, x + 110, x + 170
    led_matrix(x, 110, title, [(p5, "5V"), (pg, "GND"), (pd, "DIN")])
    ytop = 266
    vwire(p5, ytop, BUS_5V, RED, 4)
    dot(p5, BUS_5V, RED)
    vwire(pg, ytop, BUS_GND, BLK, 4, hops=[BUS_5V])
    dot(pg, BUS_GND, BLK)
    cap_h(p5, pg, 290, "1000 µF ≥ 6.3 V")
    # data line: down through both buses, resistor, then to the Mega pin
    vwire(pd, ytop, 392, LED, 3, hops=[BUS_5V, BUS_GND])
    resistor_v(pd, 392, "330 Ω")
    path(f"M{pd},436 L{pd},{jog_y} L{din_to_x},{jog_y} L{din_to_x},{din_to_y}", LED, 3)


MEGA_X, MEGA_Y, MEGA_W, MEGA_H = 500, 520, 600, 300
MEGA_TOP = MEGA_Y

matrix_block(880, "Front LED matrix", 960, MEGA_TOP, 462)
matrix_block(1260, "Back LED matrix", 1030, MEGA_TOP, 484)

# ---------------------------------------------------------------- Mega
add(f'<rect x="{MEGA_X}" y="{MEGA_Y}" width="{MEGA_W}" height="{MEGA_H}" rx="10" '
    f'fill="#0b6e8a" stroke="#064a5d" stroke-width="3"/>')
add(f'<rect x="{MEGA_X + 230}" y="{MEGA_Y + 200}" width="140" height="70" rx="4" fill="#1d1d1d"/>')
text(MEGA_X + 300, MEGA_Y + 241, "ATmega2560", 12, anchor="middle", fill="#bbb")
text(MEGA_X + MEGA_W / 2, MEGA_Y + 140, "Arduino Mega 2560", 28, anchor="middle", weight="bold", fill="#fff")
text(MEGA_X + MEGA_W / 2, MEGA_Y + 168, "powered through its 5V pin", 13, anchor="middle", fill="#d6eef5")
add(f'<rect x="{MEGA_X - 14}" y="{MEGA_Y + 230}" width="34" height="40" fill="#bbb" stroke="#666"/>')
text(MEGA_X + 30, MEGA_Y + 290, "USB-B: programming only", 11, fill="#d6eef5")

for px, lab in ((560, "5V"), (620, "GND"), (960, "D6"), (1030, "D7")):
    add(f'<rect x="{px - 7}" y="{MEGA_TOP - 4}" width="14" height="10" fill="#222"/>')
    text(px, MEGA_TOP + 26, lab, 13, anchor="middle", weight="bold", fill="#fff")

vwire(560, BUS_5V, MEGA_TOP, RED, 4, hops=[BUS_GND])
dot(560, BUS_5V, RED)
vwire(620, BUS_GND, MEGA_TOP, BLK, 4)
dot(620, BUS_GND, BLK)

# side serial pins
side = [(MEGA_X, 640, "RX3 · 15"), (MEGA_X, 720, "TX3 · 14"),
        (MEGA_X + MEGA_W, 640, "RX2 · 17"), (MEGA_X + MEGA_W, 720, "TX2 · 16")]
for sx, sy, lab in side:
    left = sx == MEGA_X
    add(f'<rect x="{sx - 4 if left else sx - 6}" y="{sy - 7}" width="10" height="14" fill="#222"/>')
    text(sx + (14 if left else -14), sy + 5, lab, 13, anchor="start" if left else "end",
         weight="bold", fill="#fff")

# ---------------------------------------------------------------- radars
RAD_Y = 560
# front radar (left)
radar(80, RAD_Y, "Front radar", "faces traffic approaching the front")
for px, lab in ((120, "5V"), (170, "GND")):
    add(f'<rect x="{px - 6}" y="{RAD_Y - 6}" width="12" height="10" fill="#c9a227"/>')
    text(px, RAD_Y + 20, lab, 12, anchor="middle", weight="bold", fill="#fff")
vwire(120, RAD_Y - 6, BUS_5V, RED, 4, hops=[BUS_GND])
dot(120, BUS_5V, RED)
vwire(170, RAD_Y - 6, BUS_GND, BLK, 4)
dot(170, BUS_GND, BLK)
for py, lab in ((640, "TX"), (720, "RX")):
    add(f'<rect x="314" y="{py - 6}" width="10" height="12" fill="#c9a227"/>')
    text(306, py + 5, lab, 12, anchor="end", weight="bold", fill="#fff")
path(f"M324,640 L{MEGA_X - 4},640", RAD, 3)
path(f"M{MEGA_X - 4},720 L452,720", RAD, 3, dash=True)
path("M368,720 L324,720", RAD, 3, dash=True)
divider_box(410, 720)
text(412, 632, "radar TX → Mega RX3", 12, anchor="middle", fill=RAD, weight="bold")

# back radar (right)
BX = 1300
radar(BX, RAD_Y, "Back radar", "faces traffic approaching the back")
p5b, pgb = 1480, 1515
for px, lab in ((p5b, "5V"), (pgb, "GND")):
    add(f'<rect x="{px - 6}" y="{RAD_Y - 6}" width="12" height="10" fill="#c9a227"/>')
    text(px, RAD_Y + 20, lab, 12, anchor="middle", weight="bold", fill="#fff")
vwire(p5b, RAD_Y - 6, BUS_5V, RED, 4, hops=[BUS_GND])
dot(p5b, BUS_5V, RED)
vwire(pgb, RAD_Y - 6, BUS_GND, BLK, 4)
dot(pgb, BUS_GND, BLK)
for py, lab in ((640, "TX"), (720, "RX")):
    add(f'<rect x="{BX - 4}" y="{py - 6}" width="10" height="12" fill="#c9a227"/>')
    text(BX + 14, py + 5, lab, 12, weight="bold", fill="#fff")
path(f"M{BX - 4},640 L{MEGA_X + MEGA_W + 4},640", RAD, 3)
path(f"M{MEGA_X + MEGA_W + 4},720 L{1200 - 42},720", RAD, 3, dash=True)
path(f"M{1200 + 42},720 L{BX - 4},720", RAD, 3, dash=True)
divider_box(1200, 720)
text(1200, 632, "radar TX → Mega RX2", 12, anchor="middle", fill=RAD, weight="bold")

text(410, 800, "optional: only needed to", 11, anchor="middle", fill="#444", italic=True)
text(410, 814, "send config to the radar", 11, anchor="middle", fill="#444", italic=True)
text(1200, 800, "optional: only needed to", 11, anchor="middle", fill="#444", italic=True)
text(1200, 814, "send config to the radar", 11, anchor="middle", fill="#444", italic=True)

# ---------------------------------------------------------------- notes
add('<rect x="40" y="850" width="1520" height="300" rx="10" fill="#f7f7f7" stroke="#ccc"/>')
text(60, 882, "Power", 17, weight="bold")
left_notes = [
    "1. Cut the device end off a USB cable. Red = +5 V, black = GND. Insulate white and",
    "    green (data) separately. Colors vary between cables — check with a multimeter.",
    "2. Use a 5 V / 3 A (or bigger) charger and a short cable with thick power wires.",
    "3. Put a 3 A fuse (blade fuse or polyfuse) in the red wire, close to the cable.",
    "4. USB can't deliver the 5 A that 512 LEDs could draw. The sketch limits the LEDs",
    "    to MAX_MILLIAMPS (2000 mA), leaving room for the radars and the Mega.",
    "5. The 5V pin bypasses the Mega's USB protection. Unplug the power cable",
    "    before you plug the Mega into a computer.",
]
for i, s in enumerate(left_notes):
    text(82 if s.startswith(" ") else 60, 910 + i * 22, s.strip(), 14)

text(820, 882, "LEDs and radar", 17, weight="bold")
right_notes = [
    "6. Put a 1000 µF (≥ 6.3 V) capacitor across 5V/GND at each matrix. Mind polarity.",
    "7. Put a 330 Ω resistor in each DIN line, close to the matrix.",
    "8. Wire to the matrix's IN end (the PCB arrows point away from it).",
    "9. Each radar needs 5 V at ≥ 300 mA. Its TX is 3.3 V and goes straight to the Mega.",
    "10. Mega TX → radar RX (dashed) is optional. If used, drop 5 V to 3.3 V.",
    "11. Every ground is joined through the GND bus.",
]
for i, s in enumerate(right_notes):
    text(820, 910 + i * 22, s, 14)

# legend
ly = 1128
items = [(RED, False, "+5 V"), (BLK, False, "GND"), (LED, False, "LED data"),
         (RAD, False, "radar UART"), (RAD, True, "optional")]
lx = 60
for color, dash, lab in items:
    da = ' stroke-dasharray="10 6"' if dash else ""
    add(f'<line x1="{lx}" y1="{ly - 5}" x2="{lx + 40}" y2="{ly - 5}" stroke="{color}" stroke-width="4"{da}/>')
    text(lx + 48, ly, lab, 13)
    lx += 48 + 9 * len(lab) + 40
dot(lx + 10, ly - 5, BLK)
text(lx + 22, ly, "wires joined", 13)
lx += 150
add(f'<path d="M{lx},{ly - 5} L{lx + 12},{ly - 5} A8,8 0 0 1 {lx + 28},{ly - 5} L{lx + 40},{ly - 5}" '
    f'fill="none" stroke="{BLK}" stroke-width="3"/>')
add(f'<line x1="{lx + 20}" y1="{ly - 20}" x2="{lx + 20}" y2="{ly + 10}" stroke="{RED}" stroke-width="3"/>')
text(lx + 50, ly, "crossing, not connected", 13)

add("</svg>")

with open(SVG_PATH, "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")
print("wrote", SVG_PATH)


# ---------------------------------------------------------------- PNG render
def find_browser():
    cands = [
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        "google-chrome", "chromium", "chromium-browser", "chrome", "msedge",
    ]
    for c in cands:
        if os.path.isfile(c) or shutil.which(c):
            return c
    return None


if "--png" in sys.argv:
    browser = find_browser()
    if not browser:
        sys.exit("No Chrome/Edge/Chromium found for PNG rendering.")
    html = os.path.join(HERE, "_render.html")
    with open(html, "w", encoding="utf-8") as f:
        f.write('<!doctype html><html><body style="margin:0">'
                f'<img src="wiring-diagram.svg" width="{W}" height="{H}" style="display:block">'
                "</body></html>")
    try:
        subprocess.run([browser, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                        f"--force-device-scale-factor={SCALE}",
                        f"--window-size={W},{H}", f"--screenshot={PNG_PATH}",
                        "file:///" + html.replace("\\", "/")], check=True)
    finally:
        os.remove(html)
    print("wrote", PNG_PATH, f"({W * SCALE}x{H * SCALE})")
