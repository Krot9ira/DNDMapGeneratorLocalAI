#!/usr/bin/env python3
"""
Blueprint preview renderer.

This draws the *human-facing* picture of a map: colour-coded areas, prop icons
and labels, so a game master (or an agent) can check the layout before spending
GPU time on it. It is never sent to the image model.

There used to be a second renderer here that produced a control image - line art
and a depth map - for a ControlNet. That is gone: Ideogram 4 takes the layout as
bounding boxes inside its JSON caption, so no control image exists any more.
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

import architect as A

# -- palette (humans only) --------------------------------------------
PREVIEW_COLORS = {
    A.VOID: (28, 30, 36),
    A.FLOOR: (232, 226, 214),
    A.WALL: (58, 62, 72),
    A.DOOR: (196, 150, 74),
    A.WINDOW: (150, 196, 214),
    A.WATER: (86, 140, 178),
    A.PIT: (34, 34, 40),
    A.RUBBLE: (176, 166, 150),
    A.VEGETATION: (118, 156, 104),
    A.STAIRS: (198, 190, 176),
    A.BRIDGE: (166, 130, 88),
}

DEFAULT_CELL_PX = 28


def _font(size):
    for path in ("C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf",
                 "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


# -- ship outline -----------------------------------------------------
# The hull also exists as tiles for walkability, but a six-cell-tall blob does
# not read as a vessel, so the preview traces the real shape.

def _hull_profile(t):
    """Half-width of the hull at position t along its length (0 stern, 1 bow)."""
    if t < 0.14:
        return 0.70 + 0.30 * (t / 0.14)
    if t < 0.58:
        return 1.0
    u = (t - 0.58) / 0.42
    return max(0.0, math.sqrt(max(0.0, 1.0 - u * u)))


def _ship_outline(st, cell):
    x, y = float(st.get("x", 0)), float(st.get("y", 0))
    w, h = float(st.get("w", 8)), float(st.get("h", 5))
    half = h / 2.0
    top, bottom = [], []
    steps = 48
    for i in range(steps + 1):
        t = i / steps
        hw = _hull_profile(t) * half
        px = (x + t * w) * cell
        top.append((px, (y + half - hw) * cell))
        bottom.append((px, (y + half + hw) * cell))
    return top + bottom[::-1]


def _draw_ship(draw, st, cell):
    outline = _ship_outline(st, cell)
    draw.polygon(outline, fill=(150, 116, 76), outline=(52, 40, 26))
    x, y = float(st.get("x", 0)), float(st.get("y", 0))
    w, h = float(st.get("w", 8)), float(st.get("h", 5))
    for i in range(1, max(3, int(h * 2))):
        py = (y + h * i / max(3, int(h * 2))) * cell
        draw.line([((x + w * 0.12) * cell, py), ((x + w * 0.88) * cell, py)],
                  fill=(122, 92, 58), width=max(1, cell // 14))
    mid = (y + h / 2.0) * cell
    r = min(w, h) * cell * 0.10
    draw.ellipse([(x + w * 0.46) * cell - r, mid - r,
                  (x + w * 0.46) * cell + r, mid + r], outline=(40, 32, 20), width=2)


# -- prop icons -------------------------------------------------------
# Chunky and distinct: at preview scale you have to be able to tell a barrel
# from a brazier at a glance.
def _draw_feature(d, feat, c, lw, color=(40, 40, 48)):
    try:
        cx = (int(feat["x"]) + 0.5) * c
        cy = (int(feat["y"]) + 0.5) * c
    except (KeyError, TypeError, ValueError):
        return
    kind = str(feat.get("kind", "pillar")).lower()
    r = c * 0.30
    W = max(1, lw)

    def circle(rad):
        d.ellipse([cx - rad, cy - rad, cx + rad, cy + rad], outline=color, width=W)

    def box(hw, hh):
        d.rectangle([cx - hw, cy - hh, cx + hw, cy + hh], outline=color, width=W)

    if kind in ("pillar", "column", "stalagmite", "bollard"):
        circle(r * 0.8)
        circle(r * 0.4)
    elif kind in ("barrel", "keg"):
        circle(r * 0.85)
        d.line([(cx - r * 0.85, cy), (cx + r * 0.85, cy)], fill=color, width=W)
    elif kind in ("crate", "box", "cargo", "chest", "locker", "cabinet"):
        box(r * 0.85, r * 0.7)
        d.line([(cx - r * 0.85, cy), (cx + r * 0.85, cy)], fill=color, width=W)
    elif kind in ("table", "desk", "workbench", "bar", "bench"):
        box(r * 1.0, r * 0.55)
    elif kind in ("chair", "stool", "seat"):
        box(r * 0.4, r * 0.4)
    elif kind in ("bed", "bunk", "cot"):
        box(r * 0.65, r * 1.0)
        d.line([(cx - r * 0.65, cy - r * 0.45), (cx + r * 0.65, cy - r * 0.45)],
               fill=color, width=W)
    elif kind in ("sarcophagus", "coffin", "tomb"):
        d.polygon([(cx - r * 0.5, cy - r), (cx + r * 0.5, cy - r),
                   (cx + r * 0.65, cy + r), (cx - r * 0.65, cy + r)],
                  outline=color, width=W)
    elif kind in ("altar", "shrine"):
        box(r * 1.0, r * 0.6)
        d.line([(cx, cy - r * 0.4), (cx, cy + r * 0.4)], fill=color, width=W)
    elif kind in ("torch", "lamp", "lantern", "sconce"):
        circle(r * 0.35)
        d.line([(cx, cy + r * 0.35), (cx, cy + r * 0.9)], fill=color, width=W)
    elif kind in ("brazier", "cauldron", "campfire", "hearth", "forge"):
        circle(r * 0.85)
        for a in range(0, 360, 60):
            d.line([(cx, cy), (cx + r * 0.55 * math.cos(math.radians(a)),
                               cy + r * 0.55 * math.sin(math.radians(a)))],
                   fill=color, width=W)
    elif kind in ("well", "fountain", "pool"):
        circle(r * 0.95)
        circle(r * 0.5)
    elif kind in ("bookshelf", "shelf", "weapon_rack", "rack"):
        box(r * 1.0, r * 0.45)
        for i in (-1, 0, 1):
            d.line([(cx + i * r * 0.45, cy - r * 0.45), (cx + i * r * 0.45, cy + r * 0.45)],
                   fill=color, width=max(1, W - 1))
    elif kind in ("statue", "obelisk", "totem", "idol"):
        d.polygon([(cx, cy - r), (cx + r * 0.6, cy + r * 0.7), (cx - r * 0.6, cy + r * 0.7)],
                  outline=color, width=W)
    elif kind in ("crystal", "gem", "shard"):
        d.polygon([(cx, cy - r), (cx + r * 0.55, cy), (cx, cy + r), (cx - r * 0.55, cy)],
                  outline=color, width=W)
    elif kind in ("tree", "bush", "shrub", "mushroom"):
        circle(r * 0.95)
        circle(r * 0.45)
    elif kind in ("boulder", "rock", "stone", "stump"):
        d.polygon([(cx - r * 0.9, cy + r * 0.3), (cx - r * 0.5, cy - r * 0.7),
                   (cx + r * 0.45, cy - r * 0.8), (cx + r * 0.9, cy + r * 0.1),
                   (cx + r * 0.3, cy + r * 0.85)], outline=color, width=W)
    elif kind in ("bones", "skeleton", "skull", "bone_pile"):
        circle(r * 0.4)
        d.line([(cx - r * 0.8, cy + r * 0.6), (cx + r * 0.8, cy + r * 0.3)],
               fill=color, width=W)
    elif kind in ("mast", "capstan"):
        circle(r * 0.5)
        d.line([(cx - r, cy), (cx + r, cy)], fill=color, width=W)
        d.line([(cx, cy - r), (cx, cy + r)], fill=color, width=W)
    elif kind in ("rope_coil", "net", "coil"):
        circle(r * 0.9)
        circle(r * 0.6)
        circle(r * 0.3)
    elif kind in ("cart", "wagon", "dumpster", "vending", "console"):
        box(r * 0.95, r * 0.6)
    elif kind in ("anvil",):
        d.polygon([(cx - r * 0.8, cy + r * 0.4), (cx + r * 0.8, cy + r * 0.4),
                   (cx + r * 0.45, cy - r * 0.2), (cx - r * 0.45, cy - r * 0.5)],
                  outline=color, width=W)
    else:
        circle(r * 0.7)
        d.line([(cx - r * 0.45, cy), (cx + r * 0.45, cy)], fill=color, width=W)


# -- the preview ------------------------------------------------------

def trim_to_margin(image_path, map_data, style=None):
    """Paint the bleed margin onto a finished render.

    The renderer is told the content is inset and usually obliges, but under a
    large effect it paints out to the frame edge. The margin is a mechanical
    thing, so it is guaranteed here rather than negotiated: the outer band is
    filled flat, like the mount around a printed map.
    """
    border = A.border_of(map_data)
    if border <= 0:
        return False
    grid_cfg = (map_data.get("grid") or {})
    cols = int(grid_cfg.get("cols", 0) or 0)
    rows = int(grid_cfg.get("rows", 0) or 0)
    if cols <= 0 or rows <= 0:
        return False

    img = Image.open(image_path).convert("RGB")
    w, h = img.size
    mx = max(1, round(w * border / cols))
    my = max(1, round(h * border / rows))

    # The mount takes its colour from the map itself, so it never looks bolted
    # on: the median of what the renderer already put in the margin ring.
    ring = []
    for x in range(0, w, max(1, w // 120)):
        ring.append(img.getpixel((x, min(my // 2, h - 1))))
        ring.append(img.getpixel((x, max(0, h - 1 - my // 2))))
    for y in range(0, h, max(1, h // 120)):
        ring.append(img.getpixel((min(mx // 2, w - 1), y)))
        ring.append(img.getpixel((max(0, w - 1 - mx // 2), y)))
    ring.sort(key=lambda c: c[0] + c[1] + c[2])
    fill = ring[len(ring) // 2] if ring else (150, 145, 125)

    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w, my], fill=fill)
    d.rectangle([0, h - my, w, h], fill=fill)
    d.rectangle([0, 0, mx, h], fill=fill)
    d.rectangle([w - mx, 0, w, h], fill=fill)
    img.save(image_path)
    return True


def render_preview(map_data, out_path=None, cell=DEFAULT_CELL_PX, show_labels=True):
    """Colour-coded blueprint with area labels, for humans and agents to read."""
    grid = A.zones_to_grid(map_data)
    w, h = grid.cols * cell, grid.rows * cell
    img = Image.new("RGB", (w, h), PREVIEW_COLORS[A.VOID])
    d = ImageDraw.Draw(img)

    for y in range(grid.rows):
        for x in range(grid.cols):
            d.rectangle([x * cell, y * cell, (x + 1) * cell - 1, (y + 1) * cell - 1],
                        fill=PREVIEW_COLORS.get(grid.get(x, y), (120, 120, 120)))

    for st in map_data.get("structures", []) or []:
        if st.get("kind") == "ship":
            _draw_ship(d, st, cell)

    # Faint cell guides so a human can count squares. A UI affordance only -
    # nothing here reaches the image model.
    for x in range(grid.cols + 1):
        d.line([(x * cell, 0), (x * cell, h)], fill=(0, 0, 0), width=1)
    for y in range(grid.rows + 1):
        d.line([(0, y * cell), (w, y * cell)], fill=(0, 0, 0), width=1)

    # The bleed margin is not part of the field, so the preview says so the same
    # way the editor does: dimmed, hatched, and fenced off with a gold line.
    border = A.border_of(map_data)
    if border > 0:
        ix0, iy0 = border * cell, border * cell
        ix1, iy1 = (grid.cols - border) * cell, (grid.rows - border) * cell
        shade = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        sd = ImageDraw.Draw(shade)
        sd.rectangle([0, 0, w, iy0], fill=(12, 13, 17, 170))
        sd.rectangle([0, iy1, w, h], fill=(12, 13, 17, 170))
        sd.rectangle([0, iy0, ix0, iy1], fill=(12, 13, 17, 170))
        sd.rectangle([ix1, iy0, w, iy1], fill=(12, 13, 17, 170))
        step = max(9, cell * 3 // 4)
        for off in range(0, w + h, step):
            sd.line([(off, 0), (off - h, h)], fill=(150, 140, 115, 40), width=1)
        img.paste(Image.alpha_composite(img.convert("RGBA"), shade).convert("RGB"), (0, 0))
        d = ImageDraw.Draw(img)
        d.rectangle([ix0, iy0, ix1 - 1, iy1 - 1], outline=(214, 186, 122),
                    width=max(2, cell // 10))

    for f in map_data.get("features", []) or []:
        _draw_feature(d, f, cell, max(1, cell // 14))

    if show_labels:
        fnt = _font(max(10, int(cell * 0.5)))
        for area in map_data.get("areas", []) or []:
            text = str(area.get("label", ""))
            if not text:
                continue
            tx = (area.get("x", 0) + area.get("w", 1) / 2.0) * cell
            ty = (area.get("y", 0) + area.get("h", 1) / 2.0) * cell
            bbox = d.textbbox((0, 0), text, font=fnt)
            tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
            d.rectangle([tx - tw / 2 - 4, ty - th / 2 - 3, tx + tw / 2 + 4, ty + th / 2 + 4],
                        fill=(20, 22, 28))
            d.text((tx - tw / 2, ty - th / 2 - 1), text, fill=(240, 220, 160), font=fnt)

    if out_path:
        Path(out_path).parent.mkdir(parents=True, exist_ok=True)
        img.save(str(out_path))
    return img


def render_svg(map_data, out_path, cell=DEFAULT_CELL_PX):
    """Vector version of the preview, handy for printing or hand-editing."""
    grid = A.zones_to_grid(map_data)
    w, h = grid.cols * cell, grid.rows * cell
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
           f'viewBox="0 0 {w} {h}">']
    r, g, b = PREVIEW_COLORS[A.VOID]
    out.append(f'<rect width="{w}" height="{h}" fill="rgb({r},{g},{b})"/>')
    for z in map_data.get("zones", []) or []:
        col = PREVIEW_COLORS.get(str(z.get("kind", A.FLOOR)), (120, 120, 120))
        out.append(
            f'<rect x="{int(z.get("x", 0)) * cell}" y="{int(z.get("y", 0)) * cell}" '
            f'width="{int(z.get("w", 1)) * cell}" height="{int(z.get("h", 1)) * cell}" '
            f'fill="rgb({col[0]},{col[1]},{col[2]})"/>')
    for st in map_data.get("structures", []) or []:
        if st.get("kind") != "ship":
            continue
        pts = " ".join(f"{px:.0f},{py:.0f}" for px, py in _ship_outline(st, cell))
        out.append(f'<polygon points="{pts}" fill="rgb(150,116,76)" '
                   f'stroke="rgb(52,40,26)" stroke-width="2"/>')
    for f in map_data.get("features", []) or []:
        try:
            cx = (int(f["x"]) + 0.5) * cell
            cy = (int(f["y"]) + 0.5) * cell
        except (KeyError, TypeError, ValueError):
            continue
        out.append(f'<circle cx="{cx}" cy="{cy}" r="{cell * 0.26:.1f}" fill="none" '
                   f'stroke="rgb(40,40,48)" stroke-width="2"/>')
    for area in map_data.get("areas", []) or []:
        label = str(area.get("label", ""))
        if not label:
            continue
        tx = (area.get("x", 0) + area.get("w", 1) / 2.0) * cell
        ty = (area.get("y", 0) + area.get("h", 1) / 2.0) * cell
        out.append(f'<text x="{tx:.0f}" y="{ty:.0f}" text-anchor="middle" '
                   f'font-family="Segoe UI, Arial" font-size="{int(cell * 0.5)}" '
                   f'fill="rgb(240,220,160)">{_escape(label)}</text>')
    out.append("</svg>")
    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    Path(out_path).write_text("\n".join(out), encoding="utf-8")


def _escape(text):
    return (text.replace("&", "&amp;").replace("<", "&lt;")
                .replace(">", "&gt;").replace('"', "&quot;"))


if __name__ == "__main__":
    import json
    import sys

    src = sys.argv[1] if len(sys.argv) > 1 else None
    data = (json.loads(Path(src).read_text(encoding="utf-8")) if src
            else A.build({"layout": "harbour", "size": "large",
                          "rooms": [{"label": "Warehouse"}, {"label": "Office"}]}, seed=4))
    render_preview(data, "output/_preview.png")
    print("wrote output/_preview.png")
