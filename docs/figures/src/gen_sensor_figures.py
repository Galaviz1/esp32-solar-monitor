#!/usr/bin/env python3
"""Draw the DHT11 + OLED wiring figures for docs/hardware.md.

    python3 docs/figures/src/gen_sensor_figures.py

Writes docs/figures/sensors-wiring.svg (the 38-pin board seen from above, with
the OLED and the DHT11 wired to it) and docs/figures/dht11-schematic.svg.
Same visual language as oled-wiring.svg / oled-schematic.svg.
"""
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent

STYLE = """<style>
svg { font-family: "IBM Plex Mono", Menlo, Consolas, monospace; }
text { fill: #17191a; font-size: 12px; }
.h { font-family: Archivo, "Helvetica Neue", Arial, sans-serif; font-weight: 700; font-size: 16px; }
.t { font-family: Archivo, "Helvetica Neue", Arial, sans-serif; font-weight: 700; font-size: 15px; }
.m { fill: #5b625e; font-size: 11px; }
.pcb { fill: #1d2733; } .can { fill: #b9bec5; stroke: #8b9097; } .usb { fill: #c8cbd0; stroke: #8b9097; }
.pl { fill: #c9d2dc; font-size: 10px; } .pu { fill: #ffd34d; font-size: 10px; font-weight: 700; }
.px { fill: #ff8a80; font-size: 10px; }
.pin { fill: #9aa3ad; } .wire { fill: none; stroke-width: 5; stroke-linecap: round; stroke-linejoin: round; }
.bad { fill: #c2382b; font-weight: 600; font-size: 11px; }
.scr { fill: #08131f; } .st { fill: #7fd3ff; font-size: 9px; }
.box { fill: #f4f5f2; stroke: #17191a; stroke-width: 1.5; }
.w { fill: none; stroke-width: 2.5; }
.new { fill: #e9f6ee; stroke: #1a8a4a; stroke-width: 1.5; }
</style>"""

RED, BLACK, YELLOW, BLUE, GREEN = "#d8352a", "#1f1f1f", "#e0a800", "#2a66c9", "#1a8a4a"

# 38-pin DevKitC layout, antenna at the top; (label, kind) per row, pin 1 first.
# kind: "" plain, "u" used here, "x" never connect.
ROW_B = [("3V3", "u"), ("EN", ""), ("VP", ""), ("VN", ""), ("34", ""), ("35", ""), ("32", ""),
         ("33", ""), ("25", ""), ("26", ""), ("27", ""), ("14", ""), ("12", ""), ("GND", ""),
         ("13", ""), ("SD2", "x"), ("SD3", "x"), ("CMD", "x"), ("5V", "x")]
ROW_A = [("GND", "u"), ("23", "u"), ("22", "u"), ("TX", ""), ("RX", ""), ("21", "u"), ("GND", "u"),
         ("19", ""), ("18", ""), ("5", ""), ("17", ""), ("16", ""), ("4", ""), ("0", ""), ("2", ""),
         ("15", ""), ("SD1", "x"), ("SD0", "x"), ("CLK", "x")]
XB, XA, Y1, DY = 232, 408, 112, 25


def py(i):  # y of pin i (1-based) in either row
    return Y1 + (i - 1) * DY


def wiring():
    o = []
    A = o.append
    A('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 860 820" role="img" aria-label="'
      "Wiring of the OLED and the DHT11 to the 38-pin USB-C ESP32 board, antenna at the top. "
      "OLED: VCC to 3V3 (row B, pin 1), GND to GND (row A, pin 1), SCL to 22 (row A, pin 3), "
      "SDA to 21 (row A, pin 6). DHT11: + to 3V3 through a Y splitter shared with the OLED VCC, "
      "minus to GND (row A, pin 7), S (data) to 23 (row A, pin 2). The flash pins and 5V at the "
      'USB end are never connected.">')
    A(STYLE)
    A('<rect width="860" height="820" fill="#ffffff"/>')
    A('<text class="h" x="20" y="24">OLED + DHT11 to ESP32: wiring</text>')
    A('<text class="m" x="20" y="41">38-pin board, USB-C, jumper wires straight onto the pins. Seen from above.</text>')

    # board
    A('<rect class="pcb" x="220" y="84" width="200" height="506" rx="8"/>')
    A('<rect class="can" x="276" y="70" width="88" height="128" rx="3"/>')
    A('<text x="320" y="128" text-anchor="middle" style="fill:#2d3238;font-size:10px;font-weight:600">ESP32-</text>')
    A('<text x="320" y="142" text-anchor="middle" style="fill:#2d3238;font-size:10px;font-weight:600">WROOM-32</text>')
    A('<text x="320" y="88" text-anchor="middle" style="fill:#2d3238;font-size:9px">antenna</text>')
    A('<rect class="usb" x="292" y="578" width="56" height="26" rx="6"/>')
    A('<text class="m" x="320" y="624" text-anchor="middle">USB-C</text>')
    A('<text class="m" x="190" y="104" text-anchor="end">row B</text>')
    A('<text class="m" x="450" y="104">row A</text>')
    for row, x, anchor, dx in ((ROW_B, XB, "start", 12), (ROW_A, XA, "end", -12)):
        for i, (lab, kind) in enumerate(row, 1):
            y = py(i)
            A(f'<circle class="pin" cx="{x}" cy="{y}" r="4.5"/>')
            cls = {"u": "pu", "x": "px"}.get(kind, "pl")
            A(f'<text class="{cls}" x="{x + dx}" y="{y + 3.5}" text-anchor="{anchor}">{lab}</text>')
            if kind == "x":
                A(f'<path d="M{x-4},{y-4} L{x+4},{y+4} M{x+4},{y-4} L{x-4},{y+4}" stroke="#ff5a4d" stroke-width="1.8"/>')
    A('<path d="M206,479 L200,479 L200,570 L206,570" fill="none" stroke="#c2382b" stroke-width="1.5"/>')
    A('<text class="bad" x="194" y="514" text-anchor="end">flash + 5V:</text>')
    A('<text class="bad" x="194" y="539" text-anchor="end">never connect</text>')
    A('<path d="M434,504 L440,504 L440,570 L434,570" fill="none" stroke="#c2382b" stroke-width="1.5"/>')
    A('<text class="bad" x="446" y="533">flash pins:</text>')
    A('<text class="bad" x="446" y="547">never connect</text>')

    # OLED module (as in oled-wiring.svg)
    A('<rect x="620" y="96" width="140" height="104" rx="6" fill="#1d2733"/>')
    A('<rect class="scr" x="652" y="118" width="98" height="48" rx="2"/>')
    A('<text class="st" x="656" y="134">24.5°C   45%</text>')
    A('<text class="st" x="656" y="147" style="font-size:7px">air temp  humidity</text>')
    A('<text class="st" x="656" y="159" style="font-size:7px">  (Environment page)</text>')
    A('<text class="t" x="690" y="222" text-anchor="middle">SSD1306</text>')
    A('<text class="m" x="690" y="238" text-anchor="middle">0.91″ 128×32 · 0x3C</text>')
    for lab, y in (("GND", 112), ("VCC", 132), ("SCL", 152), ("SDA", 172)):
        A(f'<circle cx="630" cy="{y}" r="3.6" fill="#d9b44a"/><text x="638" y="{y+3}" style="fill:#e6e9ee;font-size:9px">{lab}</text>')

    # DHT11 module, pins on top: S, +, -
    A('<rect x="590" y="520" width="170" height="96" rx="6" fill="#1d2733"/>')
    A('<rect x="636" y="546" width="78" height="62" rx="4" fill="#3d8fd6"/>')
    for k in range(5):
        yy = 553 + k * 11
        A(f'<rect x="644" y="{yy}" width="62" height="5" rx="2" fill="#2a6fae"/>')
    A('<circle cx="612" cy="590" r="6" fill="#d24a3a"/>')  # power LED, typical on these modules
    pins = {"S": 620, "+": 675, "−": 730}
    for lab, x in pins.items():
        A(f'<circle cx="{x}" cy="520" r="3.6" fill="#d9b44a"/>')
        A(f'<text x="{x}" y="538" text-anchor="middle" style="fill:#e6e9ee;font-size:10px;font-weight:700">{lab}</text>')
    A('<text class="t" x="675" y="638" text-anchor="middle">DHT11 module</text>')
    A('<text class="m" x="675" y="654" text-anchor="middle">temp + humidity · run it on 3.3 V</text>')
    A('<text class="m" x="675" y="669" text-anchor="middle">pull-up resistor on the module</text>')

    def wire(color, d, a, b):
        A(f'<path class="wire" stroke="{color}" d="{d}"/>')
        for (x, y) in (a, b):
            A(f'<circle cx="{x}" cy="{y}" r="3.4" fill="#3a3a36"/>')

    # OLED wires (unchanged from oled-wiring.svg); VCC now runs via the splitter
    wire(BLACK, "M408,112 C480,112 560,112 630,112", (408, 112), (630, 112))
    wire(RED, "M232,112 C202,112 202,58 270,58 L560,58 C600,58 596,132 630,132", (232, 112), (630, 132))
    wire(YELLOW, "M408,162 C480,162 560,152 630,152", (408, 162), (630, 152))
    wire(BLUE, "M408,237 C490,237 560,172 630,172", (408, 237), (630, 172))

    # DHT11 wires
    wire(GREEN, "M408,137 L480,137 Q500,137 500,157 L500,470 Q500,490 520,490 L600,490 Q620,490 620,505 L620,520",
         (408, 137), (620, 520))                                                   # S -> 23 (A2)
    wire(BLACK, "M408,262 L770,262 Q800,262 800,282 L800,480 Q800,500 780,500 L745,500 Q730,500 730,510 L730,520",
         (408, 262), (730, 520))                                                   # - -> GND (A7)
    wire(RED, "M580,58 L580,48 L815,48 Q830,48 830,63 L830,450 Q830,470 810,470 L690,470 Q675,470 675,485 L675,520",
         (580, 48), (675, 520))                                                    # + -> 3V3 via splitter
    # the splitter: one 3V3 pin, two wires
    A('<rect x="565" y="40" width="30" height="26" rx="5" fill="#fff4f2" stroke="#c2382b" stroke-width="1.5"/>')
    A('<text x="580" y="57" text-anchor="middle" style="fill:#c2382b;font-size:10px;font-weight:700">Y</text>')
    A('<text class="m" x="602" y="34">3V3 splitter: one pin, two wires</text>')

    # legend
    A('<text class="t" x="20" y="712">Connections</text>')
    rows = [
        (RED, "OLED VCC → 3V3 (B1)", RED, "DHT11 + → 3V3 (B1, via the splitter)"),
        (BLACK, "OLED GND → GND (A1)", BLACK, "DHT11 − → GND (A7)"),
        (YELLOW, "OLED SCL → 22 (A3)", GREEN, "DHT11 S (data) → 23 (A2)"),
        (BLUE, "OLED SDA → 21 (A6)", None, ""),
    ]
    for k, (c1, t1, c2, t2) in enumerate(rows):
        y = 736 + k * 22
        A(f'<line x1="20" y1="{y}" x2="44" y2="{y}" stroke="{c1}" stroke-width="5" stroke-linecap="round"/><text x="54" y="{y+4}">{t1}</text>')
        if c2:
            A(f'<line x1="330" y1="{y}" x2="354" y2="{y}" stroke="{c2}" stroke-width="5" stroke-linecap="round"/><text x="364" y="{y+4}">{t2}</text>')
    A('<text class="m" x="330" y="806">B1 / A2 … = row, pin counted from the antenna end. Match the printed labels.</text>')
    A("</svg>")
    return "\n".join(o)


def schematic():
    o = []
    A = o.append
    A('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 860 380" role="img" aria-label="'
      "Schematic: the ESP32's 3V3 pin (row B, pin 1) feeds both the OLED VCC and the DHT11 plus pin. "
      "GND (row A, pin 7) goes to the DHT11 minus pin. GPIO23 (row A, pin 2) goes to the DHT11 S data pin, "
      "which has a pull-up resistor to 3.3 V on the module. The OLED keeps its own GND, SDA and SCL wires. "
      'Nothing runs at 5 V.">')
    A(STYLE)
    A('<rect width="860" height="380" fill="#ffffff"/>')
    A('<text class="h" x="30" y="34">DHT11 to ESP32: schematic</text>')
    A('<text class="m" x="30" y="52">3 wires, all 3.3 V. The OLED (see oled-schematic.svg) shares only the 3V3 pin.</text>')

    A('<rect class="box" x="40" y="80" width="250" height="250" rx="6"/>')
    A('<text class="t" x="165" y="108" text-anchor="middle">ESP32, 38-pin USB-C</text>')
    A('<text class="m" x="165" y="125" text-anchor="middle">WROOM-32 · DevKitC layout</text>')

    def net(y, cls, label, color, x2=560):
        A(f'<text class="{cls}" x="280" y="{y+4}" text-anchor="end" style="fill:{color};font-weight:600">{label}</text>')
        A(f'<line class="w" stroke="{color}" x1="290" y1="{y}" x2="{x2}" y2="{y}"/>')
        A(f'<circle cx="290" cy="{y}" r="3.5" fill="{color}"/>')

    # 3V3 net, with a branch up to the OLED
    net(170, "", "3V3 · B1", RED)
    A(f'<circle cx="400" cy="170" r="4.5" fill="{RED}"/>')
    A(f'<line class="w" stroke="{RED}" x1="400" y1="170" x2="400" y2="96"/><line class="w" stroke="{RED}" x1="400" y1="96" x2="420" y2="96"/>')
    A('<rect class="box" x="420" y="74" width="120" height="44" rx="6"/>')
    A('<text class="t" x="480" y="94" text-anchor="middle">SSD1306</text>')
    A('<text class="m" x="480" y="110" text-anchor="middle">VCC (OLED)</text>')
    A('<text class="m" x="408" y="188">splitter</text>')
    net(240, "", "23 · A2", GREEN)
    net(300, "", "GND · A7", BLACK)
    A('<text class="m" x="470" y="230" text-anchor="middle">data (single wire)</text>')
    A('<text class="m" x="490" y="160" text-anchor="middle">3.3 V</text>')
    A('<text class="m" x="470" y="290" text-anchor="middle">0 V</text>')

    # DHT11 module with its on-board pull-up
    A('<rect class="new" x="560" y="110" width="270" height="230" rx="6"/>')
    A('<text class="t" x="695" y="134" text-anchor="middle">DHT11 module</text>')
    A('<text class="m" x="695" y="150" text-anchor="middle">20–90 %RH ±5 · 0–50 °C ±2</text>')
    for lab, y, c in (("+", 170, RED), ("S", 240, GREEN), ("−", 300, BLACK)):
        A(f'<circle cx="560" cy="{y}" r="3.5" fill="{c}"/><text x="570" y="{y-7}" style="fill:{c};font-weight:700">{lab}</text>')
    # pull-up resistor between + and S, inside the module
    A(f'<line class="w" stroke="{RED}" x1="560" y1="170" x2="610" y2="170"/>')
    A('<path d="M610,170 L610,186 L602,190 L618,196 L602,202 L618,208 L602,214 L610,218 L610,240" fill="none" stroke="#17191a" stroke-width="1.8"/>')
    A(f'<line class="w" stroke="{GREEN}" x1="560" y1="240" x2="730" y2="240"/>')
    A('<text class="m" x="624" y="206">pull-up</text>')
    A('<text class="m" x="624" y="220">(on module)</text>')
    A('<rect x="730" y="214" width="84" height="104" rx="4" fill="#3d8fd6"/>')
    A('<text x="772" y="262" text-anchor="middle" style="fill:#ffffff;font-weight:700">DHT11</text>')
    A('<text x="772" y="278" text-anchor="middle" style="fill:#e6f0fb;font-size:10px">sensor</text>')
    A(f'<line class="w" stroke="{BLACK}" x1="560" y1="300" x2="730" y2="300"/>')

    A('<text class="m" x="40" y="356">⚠ 3V3, never 5V: the pull-up would put 5 V on GPIO23.</text>')
    A('<text class="m" x="830" y="356" text-anchor="end">pin order varies: match the S / + / − labels</text>')
    A("</svg>")
    return "\n".join(o)


if __name__ == "__main__":
    (OUT / "sensors-wiring.svg").write_text(wiring() + "\n")
    (OUT / "dht11-schematic.svg").write_text(schematic() + "\n")
    print("wrote", OUT / "sensors-wiring.svg", "and", OUT / "dht11-schematic.svg")
