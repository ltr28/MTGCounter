#!/usr/bin/env python3
"""Generates firmware/MTGCounter/counter_icons.c/.h - the symbols used for
the per-player extra counters (poison, energy, mana, ...).

Each icon is drawn here with simple vector shapes at a high resolution, then
box-downsampled to the two sizes the UI uses (SMALL for the on-wedge counter
chips, LARGE for the counter picker grid) and emitted as LVGL 8
LV_IMG_CF_ALPHA_8BIT images. Alpha-only images carry no colour of their own:
the firmware tints them at draw time with the `img_recolor` style, so one
bitmap serves every colour (e.g. the commander-damage shield is drawn in the
source player's colour).

Requires Pillow. Re-run after editing a shape:

    python3 firmware/tools/gen_counter_icons.py
"""
import math
import os

from PIL import Image, ImageDraw

SMALL = 16
LARGE = 30
CANVAS = 240  # draw resolution; both output sizes divide evenly into it

ON = 255
OFF = 0


def P(x, y):
    """Unit coords (0..1) -> canvas pixels."""
    return (x * CANVAS, y * CANVAS)


def poly(d, pts, fill=ON):
    d.polygon([P(x, y) for x, y in pts], fill=fill)


def ellipse(d, cx, cy, rx, ry, fill=ON):
    d.ellipse([P(cx - rx, cy - ry), P(cx + rx, cy + ry)], fill=fill)


def rect(d, x0, y0, x1, y1, fill=ON, r=0.0):
    if r:
        d.rounded_rectangle([P(x0, y0), P(x1, y1)], radius=r * CANVAS, fill=fill)
    else:
        d.rectangle([P(x0, y0), P(x1, y1)], fill=fill)


def thick_line(d, x0, y0, x1, y1, w, fill=ON):
    """A line with round caps, `w` in unit coords."""
    d.line([P(x0, y0), P(x1, y1)], fill=fill, width=int(w * CANVAS))
    ellipse(d, x0, y0, w / 2, w / 2, fill)
    ellipse(d, x1, y1, w / 2, w / 2, fill)


# ---- shapes -----------------------------------------------------------------


def draw_poison(d):
    # A phi-style glyph: a ring crossed by a vertical bar.
    ellipse(d, 0.5, 0.5, 0.36, 0.28)
    ellipse(d, 0.5, 0.5, 0.22, 0.15, OFF)
    rect(d, 0.42, 0.06, 0.58, 0.94, r=0.04)


def bezier(p0, p1, p2, steps=16):
    """Quadratic bezier from p0 to p2 (control p1), excluding p0."""
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        out.append(((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
                    (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]))
    return out


def draw_commander(d):
    # Heater shield with a cut-in border.
    def shield(inset):
        l, r, top, bot, mid = 0.12 + inset, 0.88 - inset, 0.06 + inset, 0.96 - inset * 1.5, 0.46
        pts = [(l, top), (0.5, top + 0.06), (r, top), (r, mid)]
        pts += bezier((r, mid), (r - 0.02, 0.80 - inset), (0.5, bot))
        pts += bezier((0.5, bot), (l + 0.02, 0.80 - inset), (l, mid))
        return pts

    poly(d, shield(0.0))
    poly(d, shield(0.08), OFF)
    poly(d, shield(0.14))


def draw_energy(d):
    poly(d, [(0.60, 0.04), (0.18, 0.56), (0.46, 0.56), (0.36, 0.96), (0.82, 0.40), (0.54, 0.40), (0.66, 0.04)])


def draw_experience(d):
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        rad = 0.46 if i % 2 == 0 else 0.19
        pts.append((0.5 + rad * math.cos(a), 0.54 + rad * math.sin(a)))
    poly(d, pts)


def draw_plus_one(d):
    rect(d, 0.08, 0.08, 0.92, 0.92, r=0.18)
    rect(d, 0.42, 0.22, 0.58, 0.78, OFF)
    rect(d, 0.22, 0.42, 0.78, 0.58, OFF)


def draw_monarch(d):
    poly(d, [(0.08, 0.30), (0.30, 0.52), (0.5, 0.18), (0.70, 0.52), (0.92, 0.30), (0.84, 0.74), (0.16, 0.74)])
    rect(d, 0.16, 0.78, 0.84, 0.90, r=0.03)
    for x, y in ((0.08, 0.26), (0.5, 0.14), (0.92, 0.26)):
        ellipse(d, x, y, 0.07, 0.07)


def draw_rad(d):
    # Trefoil: three 60-degree annular sectors around a centre dot.
    c = P(0.5, 0.5)
    for start in (-120, 0, 120):
        a0, a1 = start - 30 - 90, start + 30 - 90
        R = 0.46 * CANVAS
        d.pieslice([c[0] - R, c[1] - R, c[0] + R, c[1] + R], a0, a1, fill=ON)
    ellipse(d, 0.5, 0.5, 0.17, 0.17, OFF)
    ellipse(d, 0.5, 0.5, 0.10, 0.10)


def draw_tally(d):
    for x in (0.20, 0.38, 0.56, 0.74):
        thick_line(d, x, 0.14, x, 0.86, 0.09)
    thick_line(d, 0.08, 0.76, 0.92, 0.24, 0.09)


def draw_mana_w(d):
    # Sun: disc + eight rays.
    for i in range(8):
        a = i * math.pi / 4
        tip = (0.5 + 0.47 * math.cos(a), 0.5 + 0.47 * math.sin(a))
        b1 = (0.5 + 0.26 * math.cos(a - 0.30), 0.5 + 0.26 * math.sin(a - 0.30))
        b2 = (0.5 + 0.26 * math.cos(a + 0.30), 0.5 + 0.26 * math.sin(a + 0.30))
        poly(d, [tip, b1, b2])
    ellipse(d, 0.5, 0.5, 0.25, 0.25)


def draw_mana_u(d):
    # Water drop: circle with a pointed top.
    ellipse(d, 0.5, 0.64, 0.30, 0.30)
    poly(d, [(0.5, 0.04), (0.215, 0.56), (0.785, 0.56)])
    ellipse(d, 0.40, 0.66, 0.07, 0.11, OFF)


def draw_mana_b(d):
    # Skull.
    ellipse(d, 0.5, 0.42, 0.36, 0.34)
    rect(d, 0.30, 0.56, 0.70, 0.88, r=0.06)
    ellipse(d, 0.36, 0.46, 0.10, 0.11, OFF)
    ellipse(d, 0.64, 0.46, 0.10, 0.11, OFF)
    poly(d, [(0.5, 0.58), (0.45, 0.68), (0.55, 0.68)], OFF)
    for x in (0.40, 0.5, 0.60):
        rect(d, x - 0.02, 0.76, x + 0.02, 0.88, OFF)


def draw_mana_r(d):
    # Flame: a round base swept up into a leaning tip, with an inner
    # cut-out and a smaller side tongue.
    def flame(cx, cy, r, tip, lean):
        left, right = (cx - r, cy), (cx + r, cy)
        pts = bezier(left, (cx - r * 0.9, cy - (cy - tip[1]) * 0.55), tip)
        pts += bezier(tip, (cx + r * 1.15 + lean, cy - (cy - tip[1]) * 0.45), right)
        for i in range(1, 24):  # lower half-circle back to the left side
            a = i / 24 * math.pi
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        return [left] + pts

    poly(d, flame(0.52, 0.66, 0.30, (0.50, 0.03), 0.0))
    poly(d, flame(0.53, 0.76, 0.13, (0.48, 0.42), 0.0), OFF)
    poly(d, flame(0.24, 0.70, 0.10, (0.16, 0.38), 0.0))


def draw_mana_g(d):
    # Pine tree: three stacked tiers and a trunk.
    poly(d, [(0.5, 0.04), (0.74, 0.34), (0.26, 0.34)])
    poly(d, [(0.5, 0.20), (0.82, 0.56), (0.18, 0.56)])
    poly(d, [(0.5, 0.38), (0.90, 0.78), (0.10, 0.78)])
    rect(d, 0.42, 0.76, 0.58, 0.96)


def draw_mana_c(d):
    # Faceted diamond outline.
    poly(d, [(0.5, 0.04), (0.92, 0.5), (0.5, 0.96), (0.08, 0.5)])
    poly(d, [(0.5, 0.22), (0.74, 0.5), (0.5, 0.78), (0.26, 0.5)], OFF)
    poly(d, [(0.5, 0.34), (0.62, 0.5), (0.5, 0.66), (0.38, 0.5)])


def draw_life(d):
    # Heart: two lobes and a point.
    ellipse(d, 0.30, 0.36, 0.22, 0.22)
    ellipse(d, 0.70, 0.36, 0.22, 0.22)
    poly(d, [(0.085, 0.42), (0.915, 0.42), (0.5, 0.92)])
    poly(d, [(0.30, 0.20), (0.70, 0.20), (0.5, 0.45)])


ICONS = [
    ("life", draw_life),
    ("poison", draw_poison),
    ("commander", draw_commander),
    ("energy", draw_energy),
    ("experience", draw_experience),
    ("plus_one", draw_plus_one),
    ("monarch", draw_monarch),
    ("rad", draw_rad),
    ("tally", draw_tally),
    ("mana_w", draw_mana_w),
    ("mana_u", draw_mana_u),
    ("mana_b", draw_mana_b),
    ("mana_r", draw_mana_r),
    ("mana_g", draw_mana_g),
    ("mana_c", draw_mana_c),
]


def render(fn, size):
    img = Image.new("L", (CANVAS, CANVAS), OFF)
    fn(ImageDraw.Draw(img))
    return img.resize((size, size), Image.BOX)


def c_array(name, img):
    data = list(img.tobytes())
    lines = []
    for i in range(0, len(data), 18):
        lines.append("    " + ", ".join("0x%02x" % v for v in data[i:i + 18]) + ",")
    w, h = img.size
    return (
        f"static const uint8_t {name}_map[] = {{\n" + "\n".join(lines) + "\n};\n"
        f"const lv_img_dsc_t {name} = {{\n"
        f"    .header.cf = LV_IMG_CF_ALPHA_8BIT,\n"
        f"    .header.always_zero = 0,\n"
        f"    .header.reserved = 0,\n"
        f"    .header.w = {w},\n"
        f"    .header.h = {h},\n"
        f"    .data_size = {w * h},\n"
        f"    .data = {name}_map,\n"
        f"}};\n"
    )


def main():
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "MTGCounter")
    banner = "// GENERATED by firmware/tools/gen_counter_icons.py - edit the script, not this file.\n"

    c_parts = [banner, '#include "counter_icons.h"\n']
    h_parts = [
        banner,
        "#ifndef COUNTER_ICONS_H\n#define COUNTER_ICONS_H\n\n#include \"lvgl.h\"\n\n",
        "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n",
        f"#define COUNTER_ICON_SMALL {SMALL}\n#define COUNTER_ICON_LARGE {LARGE}\n\n",
        "// 8-bit alpha masks - tint with the img_recolor style.\n",
    ]
    for name, fn in ICONS:
        for suffix, size in (("s", SMALL), ("l", LARGE)):
            sym = f"icon_{name}_{suffix}"
            c_parts.append("\n" + c_array(sym, render(fn, size)))
            h_parts.append(f"extern const lv_img_dsc_t {sym};\n")
    h_parts.append("\n#ifdef __cplusplus\n}\n#endif\n\n#endif\n")

    with open(os.path.join(out_dir, "counter_icons.c"), "w") as f:
        f.write("".join(c_parts))
    with open(os.path.join(out_dir, "counter_icons.h"), "w") as f:
        f.write("".join(h_parts))

    # Contact sheet for eyeballing the shapes (not used by the firmware).
    sheet_path = os.environ.get("ICON_SHEET")
    if sheet_path:
        cell = 72
        sheet = Image.new("RGB", (cell * len(ICONS), cell * 2), (30, 30, 30))
        for i, (name, fn) in enumerate(ICONS):
            big = render(fn, 64)
            sheet.paste((255, 255, 255), (i * cell + 4, 4), big)
            for j, size in enumerate((SMALL, LARGE)):
                small = render(fn, size)
                sheet.paste((255, 255, 255), (i * cell + 4 + j * 34, cell + 10), small)
        sheet.save(sheet_path)


if __name__ == "__main__":
    main()
