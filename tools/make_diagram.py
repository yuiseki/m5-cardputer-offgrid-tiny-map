#!/usr/bin/env python3
"""Deterministically generate the article's architecture diagram.

Hackster.io doesn't render Mermaid, so this makes a PNG instead.
Uses the same colors and typefaces as make_cover.py to keep a consistent
look across the whole article.
(The colors, typefaces, and box/arrow helpers are shared with
stampfly-display's make_diagram.py.)

Usage:
    python3 tools/make_diagram.py <output-dir>

Output:
    <output-dir>/diagram-data-path.png   the staged data path that never holds a whole tile
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

# Keep these in sync with make_cover.py
BG = (18, 21, 25)
INK = (255, 255, 255)
INK_DIM = (170, 180, 190)
ACCENT = (0, 229, 160)          # the board's green LED
ACCENT2 = (0, 194, 255)         # the LCD's cyan
PLATE = (28, 33, 39)
F_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
F_REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
F_MONO = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"


def font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default()


def tsize(d, s, f):
    l, t, r, b = d.textbbox((0, 0), s, font=f)
    return r - l, b - t


def center_text(d, s, f, cx, cy, fill):
    w, h = tsize(d, s, f)
    d.text((cx - w // 2, cy - h // 2), s, font=f, fill=fill)


def box(d, x, y, w, h, title, lines, accent):
    d.rounded_rectangle([x, y, x + w, y + h], radius=14, fill=PLATE,
                        outline=accent, width=3)
    d.rectangle([x, y, x + w, y + 6], fill=accent)
    ft = font(F_BOLD, 34)
    center_text(d, title, ft, x + w // 2, y + 44, INK)
    fl = font(F_REG, 24)
    yy = y + 80
    for ln in lines:
        center_text(d, ln, fl, x + w // 2, yy, INK_DIM)
        yy += 32


def arrow(d, x1, y1, x2, y2, color, label=None, width=5):
    d.line([(x1, y1), (x2, y2)], fill=color, width=width)
    # arrowhead
    import math
    ang = math.atan2(y2 - y1, x2 - x1)
    L, S = 20, 9
    for s in (+1, -1):
        d.line([(x2, y2),
                (x2 - L * math.cos(ang) + s * S * math.sin(ang),
                 y2 - L * math.sin(ang) - s * S * math.cos(ang))],
               fill=color, width=width)
    if label:
        f = font(F_MONO, 22)
        mx, my = (x1 + x2) // 2, (y1 + y2) // 2
        w, h = tsize(d, label, f)
        d.rectangle([mx - w // 2 - 8, my - h // 2 - 6, mx + w // 2 + 8, my + h // 2 + 6],
                    fill=BG)
        center_text(d, label, f, mx, my, color)


def data_path(out):
    W, H = 1600, 1600
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)

    center_text(d, "The data path holds no whole tile", font(F_BOLD, 46), W // 2, 62, INK)
    center_text(d, "Every stage streams through a small window, from card to screen",
                font(F_REG, 26), W // 2, 116, INK_DIM)

    # A single vertical pipeline. Endpoints (storage/screen) are cyan, processing stages are green
    w, bh, step = 680, 140, 180
    x = (W - w) // 2
    cx = x + w // 2
    y0 = 165
    stages = [
        ("microSD  (FAT32)",       ["planet in 2 GiB pieces", "map.pmtiles.000 .. 038"], ACCENT2),
        ("Segmented reader",       ["global offset ->", "chunk + local offset"],         ACCENT),
        ("Streaming gzip inflate", ["never the full output"],                             ACCENT),
        ("Streaming MVT decoder",  ["never the full tile"],                               ACCENT),
        ("Vector rasterizer",      ["a strip at a time"],                                 ACCENT),
        ("RGB565 tile cache",      ["176 x 176, rendered once, on SD"],                   ACCENT2),
        ("Cardputer LCD 240x135",  ["two rows at a time, upscaled"],                      ACCENT2),
    ]
    labels = ["offset", "(chunk, local)", "32 KB window", "one ring", "one strip", "two rows"]

    for i, (title, lines, col) in enumerate(stages):
        y = y0 + i * step
        box(d, x, y, w, bh, title, lines, col)
        if i > 0:
            prev_bottom = y0 + (i - 1) * step + bh
            arrow(d, cx, prev_bottom, cx, y, ACCENT, labels[i - 1])

    # While the map is being drawn, the radio is free to scan (a separate path from the tile pipeline)
    fy = y0 + len(stages) * step + 6
    center_text(d, "While the map draws from the card, the radio stays free:",
                font(F_REG, 26), W // 2, fy, INK_DIM)
    center_text(d, "GPS -> position     Keyboard -> pan / zoom     Wi-Fi scan -> WiGLE",
                font(F_MONO, 24), W // 2, fy + 46, ACCENT2)
    center_text(d, "no stage ever holds a full map tile in RAM",
                font(F_MONO, 22), W // 2, fy + 96, (120, 132, 145))

    im.save(out)
    return out


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(outdir, exist_ok=True)
    print(data_path(os.path.join(outdir, "diagram-data-path.png")))
    return 0


sys.exit(main())
