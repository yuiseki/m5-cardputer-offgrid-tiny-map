#!/usr/bin/env python3
"""Deterministically generate the Hackster.io cover image.

So other projects can reuse the same look, the style definitions (STYLE) are
gathered at the top, and only the layout is swapped out. No randomness is
used at all.

Hackster's cover image spec (as of 2026-08, per the submission form):
    "It will be cropped to fit 4:3 format. 900x450px or higher recommended."
    -> **Build it at 4:3**. The help page says 808x632 (1.278:1), but the
      form explicitly states 4:3, and matching that avoids having the top
      and bottom cropped off.
    The source photo is 4080x3072 = 4:3, which also means no cropping is
    needed.
Output is 1600x1200 (4:3, well above the recommended minimum of 900x450).

Usage:
    python3 tools/make_cover.py <photo> <output-dir> \
        --title "M5Stamp Flying Tiny Map" \
        --subtitle "Controlling a digital map with a StampFly drone" \
        --eyebrow "M5STACK / DRONE / MAP"

Output:
    <output-dir>/cover-01-bottom-bar.png  ... each pattern
    <output-dir>/contact-sheet.png        ... overview (for picking)
"""

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------------------
# Style definitions. Change only this block to keep the look consistent
# across every project.
# ---------------------------------------------------------------------------
STYLE = {
    # Output size
    "size": (1600, 1200),   # 4:3. The form explicitly states it crops to 4:3
    # Margin. Distance to keep text away from the edges (px)
    "margin": 96,
    # Colors
    "ink": (255, 255, 255),            # primary text
    "ink_dim": (198, 205, 212),        # secondary text
    "accent": (0, 229, 160),           # accent (matched to the board's green LED)
    "accent_alt": (0, 194, 255),       # secondary accent (the LCD's cyan)
    "plate": (10, 14, 18),             # backing plate color
    # Opacity of the scrim (darkening overlay for readability)
    "scrim_strong": 235,
    "scrim_mid": 170,
    "scrim_soft": 110,
    # Fonts. DejaVu ships by default on Debian-based systems
    "font_bold": "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "font_regular": "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "font_mono": "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    # Photo focus point (normalized coordinates). Cropping centers on this point
    "focus": (0.50, 0.46),
    # Photo preprocessing
    "photo_darken": 0.10,              # slightly darken the whole image so text sits on top more easily
}


# ---------------------------------------------------------------------------
# Basic utilities
# ---------------------------------------------------------------------------
def load_font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default()


def text_size(draw, s, font):
    l, t, r, b = draw.textbbox((0, 0), s, font=font)
    return r - l, b - t


def fit_font(draw, s, path, max_w, start, floor=18):
    """Return the largest font size at which the string fits within max_w."""
    size = start
    while size > floor:
        f = load_font(path, size)
        if text_size(draw, s, f)[0] <= max_w:
            return f
        size -= 2
    return load_font(path, floor)


def cover_crop(im, out_w, out_h, focus):
    """Crop to fill the frame while preserving aspect ratio, centered on focus."""
    src_ratio = im.width / im.height
    dst_ratio = out_w / out_h
    if src_ratio > dst_ratio:
        # Source is wider; trim left/right
        new_w = int(round(im.height * dst_ratio))
        cx = focus[0] * im.width
        left = int(round(cx - new_w / 2))
        left = max(0, min(im.width - new_w, left))
        box = (left, 0, left + new_w, im.height)
    else:
        # Source is taller; trim top/bottom
        new_h = int(round(im.width / dst_ratio))
        cy = focus[1] * im.height
        top = int(round(cy - new_h / 2))
        top = max(0, min(im.height - new_h, top))
        box = (0, top, im.width, top + new_h)
    return im.crop(box).resize((out_w, out_h), Image.LANCZOS)


def vgradient(w, h, rgb, a_top, a_bottom):
    """A solid-color layer with a vertical alpha gradient."""
    layer = Image.new("RGBA", (w, h), rgb + (0,))
    d = ImageDraw.Draw(layer)
    for y in range(h):
        t = y / max(1, h - 1)
        a = int(round(a_top + (a_bottom - a_top) * t))
        d.line([(0, y), (w, y)], fill=rgb + (a,))
    return layer


def hgradient(w, h, rgb, a_left, a_right):
    layer = Image.new("RGBA", (w, h), rgb + (0,))
    d = ImageDraw.Draw(layer)
    for x in range(w):
        t = x / max(1, w - 1)
        a = int(round(a_left + (a_right - a_left) * t))
        d.line([(x, 0), (x, h)], fill=rgb + (a,))
    return layer


def darken(im, amount):
    if amount <= 0:
        return im
    layer = Image.new("RGBA", im.size, (0, 0, 0, int(round(255 * amount))))
    return Image.alpha_composite(im.convert("RGBA"), layer)


# ---------------------------------------------------------------------------
# Each layout. All of them take (base_rgba, title, subtitle, eyebrow) and
# return RGBA
# ---------------------------------------------------------------------------
def _draw_title_block(img, x, y, maxw, title, subtitle, eyebrow, align="left",
                      title_start=118, rule=True, accent=None):
    """Stack eyebrow / title (up to 2 lines) / rule / subtitle vertically. Returns the final y."""
    S = STYLE
    accent = accent or S["accent"]
    d = ImageDraw.Draw(img)

    def place(s, font, fill):
        nonlocal y
        w, h = text_size(d, s, font)
        px = x if align == "left" else (x + maxw - w if align == "right" else x + (maxw - w) // 2)
        d.text((px, y), s, font=font, fill=fill)
        y += h

    if eyebrow:
        f = load_font(S["font_mono"], 34)
        place(eyebrow, f, accent)
        y += 22

    # Wrap the title onto 2 lines (word-based). If it fits on one line, leave it as is
    ftitle = fit_font(d, title, S["font_bold"], maxw, title_start)
    if text_size(d, title, ftitle)[0] > maxw * 0.99:
        words = title.split()
        best = None
        for i in range(1, len(words)):
            a, b = " ".join(words[:i]), " ".join(words[i:])
            fa = fit_font(d, a, S["font_bold"], maxw, title_start)
            fb = fit_font(d, b, S["font_bold"], maxw, title_start)
            size = min(fa.size, fb.size)
            if best is None or size > best[0]:
                best = (size, a, b)
        f = load_font(S["font_bold"], best[0])
        for line in (best[1], best[2]):
            place(line, f, S["ink"])
            y += 4
    else:
        place(title, ftitle, S["ink"])

    if rule:
        y += 26
        rw = int(maxw * 0.34)
        rx = x if align == "left" else (x + maxw - rw if align == "right" else x + (maxw - rw) // 2)
        d.rectangle([rx, y, rx + rw, y + 7], fill=accent)
        y += 7

    if subtitle:
        y += 26
        fs = fit_font(d, subtitle, S["font_regular"], maxw, 42, floor=22)
        place(subtitle, fs, S["ink_dim"])
    return y


def v01_bottom_bar(base, title, subtitle, eyebrow):
    """An opaque bar at the bottom. The most solid choice; still readable even scaled down small."""
    S = STYLE
    W, H = base.size
    img = base.copy()
    bar_h = int(H * 0.34)
    bar = Image.new("RGBA", (W, bar_h), S["plate"] + (S["scrim_strong"],))
    img.alpha_composite(bar, (0, H - bar_h))
    d = ImageDraw.Draw(img)
    d.rectangle([0, H - bar_h, W, H - bar_h + 8], fill=S["accent"])
    _draw_title_block(img, S["margin"], H - bar_h + 58, W - 2 * S["margin"],
                      title, subtitle, eyebrow, rule=False)
    return img


def v02_bottom_scrim(base, title, subtitle, eyebrow):
    """A gradient rising from the bottom. Keeps text readable without hiding the photo."""
    S = STYLE
    W, H = base.size
    img = base.copy()
    g = vgradient(W, int(H * 0.62), (0, 0, 0), 0, 242)
    img.alpha_composite(g, (0, H - g.height))
    _draw_title_block(img, S["margin"], int(H * 0.58), W - 2 * S["margin"],
                      title, subtitle, eyebrow)
    return img


def v03_top_left(base, title, subtitle, eyebrow):
    """Stacked in the top-left. Works well when the subject sits toward the bottom-center."""
    S = STYLE
    W, H = base.size
    img = base.copy()
    g = vgradient(W, int(H * 0.55), (0, 0, 0), 232, 0)
    img.alpha_composite(g, (0, 0))
    _draw_title_block(img, S["margin"], S["margin"], int(W * 0.62),
                      title, subtitle, eyebrow, title_start=104)
    return img


def v04_left_panel(base, title, subtitle, eyebrow):
    """A vertical panel on the left. Use when you want to show the right side of the photo."""
    S = STYLE
    W, H = base.size
    img = base.copy()
    pw = int(W * 0.46)
    panel = Image.new("RGBA", (pw, H), S["plate"] + (238,))
    img.alpha_composite(panel, (0, 0))
    img.alpha_composite(hgradient(int(W * 0.12), H, (10, 14, 18), 238, 0), (pw, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([pw - 8, 0, pw, H], fill=S["accent"])
    _draw_title_block(img, S["margin"], int(H * 0.26), pw - 2 * S["margin"],
                      title, subtitle, eyebrow, title_start=88)
    return img


def v05_center_plate(base, title, subtitle, eyebrow):
    """A rounded plate in the center. The most eye-catching, but covers a large part of the photo."""
    S = STYLE
    W, H = base.size
    img = darken(base, 0.28)
    pw, ph = int(W * 0.78), int(H * 0.46)
    px, py = (W - pw) // 2, (H - ph) // 2
    plate = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
    ImageDraw.Draw(plate).rounded_rectangle([0, 0, pw - 1, ph - 1], radius=28,
                                            fill=S["plate"] + (226,))
    img.alpha_composite(plate, (px, py))
    _draw_title_block(img, px + 56, py + 54, pw - 112,
                      title, subtitle, eyebrow, align="center", title_start=104)
    return img


def v06_corner_badge(base, title, subtitle, eyebrow):
    """Title bottom-left, a small badge top-right. Leaves the most of the photo visible."""
    S = STYLE
    W, H = base.size
    img = base.copy()
    img.alpha_composite(vgradient(W, int(H * 0.5), (0, 0, 0), 0, 232), (0, H - int(H * 0.5)))
    d = ImageDraw.Draw(img)
    if eyebrow:
        f = load_font(S["font_mono"], 32)
        tw, th = text_size(d, eyebrow, f)
        bx, by = W - S["margin"] - tw - 40, S["margin"]
        d.rounded_rectangle([bx, by, bx + tw + 40, by + th + 28], radius=8,
                            fill=S["accent"])
        d.text((bx + 20, by + 13), eyebrow, font=f, fill=(6, 20, 16))
    _draw_title_block(img, S["margin"], int(H * 0.62), int(W * 0.78),
                      title, None, None, title_start=112)
    if subtitle:
        fs = fit_font(d, subtitle, S["font_regular"], int(W * 0.78), 40, floor=22)
        d.text((S["margin"], H - S["margin"] - text_size(d, subtitle, fs)[1]),
               subtitle, font=fs, fill=S["ink_dim"])
    return img


def v07_full_vignette(base, title, subtitle, eyebrow):
    """Darken the whole image with large centered text. Best visibility as a thumbnail."""
    S = STYLE
    W, H = base.size
    img = darken(base, 0.46)
    _draw_title_block(img, S["margin"], int(H * 0.30), W - 2 * S["margin"],
                      title, subtitle, eyebrow, align="center", title_start=140)
    return img


def v08_diagonal(base, title, subtitle, eyebrow):
    """A diagonal accent panel at the bottom-left. Serves as a visual marker next to other variants."""
    S = STYLE
    W, H = base.size
    img = base.copy()
    shade = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(shade).polygon([(0, int(H * 0.34)), (W, int(H * 0.66)), (W, H), (0, H)],
                                  fill=S["plate"] + (234,))
    img.alpha_composite(shade)
    line = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(line).line([(0, int(H * 0.34)), (W, int(H * 0.66))],
                              fill=S["accent"] + (255,), width=9)
    img.alpha_composite(line)
    _draw_title_block(img, S["margin"], int(H * 0.60), int(W * 0.80),
                      title, subtitle, eyebrow, rule=False)
    return img


def v09_frame(base, title, subtitle, eyebrow):
    """An inset frame. Use when you want the photo to read as a piece of art."""
    S = STYLE
    W, H = base.size
    img = darken(base, 0.16)
    m = 40
    d = ImageDraw.Draw(img)
    d.rectangle([m, m, W - m, H - m], outline=S["ink"] + (210,), width=4)
    band_h = int(H * 0.24)
    band = Image.new("RGBA", (W - 2 * m - 8, band_h), S["plate"] + (232,))
    img.alpha_composite(band, (m + 4, H - m - band_h - 4))
    _draw_title_block(img, m + 56, H - m - band_h + 26, W - 2 * m - 112,
                      title, subtitle, None, align="center", title_start=92, rule=False)
    if eyebrow:
        f = load_font(S["font_mono"], 30)
        tw, _ = text_size(d, eyebrow, f)
        d.text(((W - tw) // 2, m + 34), eyebrow, font=f, fill=S["accent"])
    return img


def v10_minimal_top(base, title, subtitle, eyebrow):
    """Just a thin strip along the top. Shows almost the entire photo."""
    S = STYLE
    W, H = base.size
    img = base.copy()
    img.alpha_composite(vgradient(W, int(H * 0.34), (0, 0, 0), 226, 0), (0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W, 10], fill=S["accent"])
    f = fit_font(d, title, S["font_bold"], W - 2 * S["margin"], 84)
    tw, th = text_size(d, title, f)
    d.text(((W - tw) // 2, 52), title, font=f, fill=S["ink"])
    if subtitle:
        fs = fit_font(d, subtitle, S["font_regular"], W - 2 * S["margin"], 36, floor=20)
        sw, _ = text_size(d, subtitle, fs)
        d.text(((W - sw) // 2, 52 + th + 18), subtitle, font=fs, fill=S["ink_dim"])
    return img


VARIANTS = [
    ("bottom-bar", v01_bottom_bar),
    ("bottom-scrim", v02_bottom_scrim),
    ("top-left", v03_top_left),
    ("left-panel", v04_left_panel),
    ("center-plate", v05_center_plate),
    ("corner-badge", v06_corner_badge),
    ("full-vignette", v07_full_vignette),
    ("diagonal", v08_diagonal),
    ("frame", v09_frame),
    ("minimal-top", v10_minimal_top),
]


def contact_sheet(paths, out, cols=3, thumb_w=520):
    """An overview image for picking a variant. Burns in numbers and names."""
    S = STYLE
    ims = [Image.open(p).convert("RGB") for p in paths]
    tw = thumb_w
    th = int(round(tw * S["size"][1] / S["size"][0]))
    label_h = 44
    rows = (len(ims) + cols - 1) // cols
    pad = 18
    W = cols * tw + (cols + 1) * pad
    H = rows * (th + label_h) + (rows + 1) * pad
    sheet = Image.new("RGB", (W, H), (24, 27, 31))
    d = ImageDraw.Draw(sheet)
    f = load_font(S["font_mono"], 26)
    for i, (im, p) in enumerate(zip(ims, paths)):
        r, c = divmod(i, cols)
        x = pad + c * (tw + pad)
        y = pad + r * (th + label_h + pad)
        sheet.paste(im.resize((tw, th), Image.LANCZOS), (x, y))
        name = os.path.basename(p).replace(".png", "")
        d.text((x + 4, y + th + 10), name, font=f, fill=(160, 230, 200))
    sheet.save(out)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("photo")
    ap.add_argument("outdir")
    ap.add_argument("--title", required=True)
    ap.add_argument("--subtitle", default=None)
    ap.add_argument("--eyebrow", default=None)
    a = ap.parse_args()

    os.makedirs(a.outdir, exist_ok=True)
    src = Image.open(a.photo).convert("RGB")
    W, H = STYLE["size"]
    base = cover_crop(src, W, H, STYLE["focus"]).convert("RGBA")
    base = darken(base, STYLE["photo_darken"])

    paths = []
    for i, (name, fn) in enumerate(VARIANTS, start=1):
        img = fn(base, a.title, a.subtitle, a.eyebrow)
        p = os.path.join(a.outdir, "cover-%02d-%s.png" % (i, name))
        img.convert("RGB").save(p, optimize=True)
        paths.append(p)
        print("%s  %d x %d" % (p, W, H))

    sheet = contact_sheet(paths, os.path.join(a.outdir, "contact-sheet.png"))
    print(sheet)
    return 0


sys.exit(main())
