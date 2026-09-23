#!/usr/bin/env python3
"""Dungeondraft Prop Foundry.

Generates custom standalone props on transparent backgrounds for map plan elements
that cannot be satisfied by the user's asset library, cuts out the background,
scales to 256px/grid cell, and packages them into a native .dungeondraft_pack.
"""
import argparse
import hashlib
import io
import json
import math
import os
import random
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

from PIL import Image, ImageDraw, ImageFilter

from paths import ROOT as PROJECT_ROOT
from dungeondraft_db import (
    AssetDatabase,
    analyze_image,
    compute_asset_id,
    save_thumbnail,
    DB_PATH_DEFAULT,
    THUMBS_DIR_DEFAULT,
)
from dungeondraft_pck import PckWriter
from dungeondraft_indexer import read_dungeondraft_config, enable_pack_in_dungeondraft_config

PACK_ID = "DBGProps01"
PACK_NAME = "DndBattlemapGenerator Custom Props"
PACK_AUTHOR = "DndBattlemapGenerator"
GENERATED_DIR_DEFAULT = PROJECT_ROOT / "data" / "generated_props"


def make_procedural_prop(
    kind: str,
    style: str = "default",
    grid_w: float = 1.0,
    grid_h: float = 1.0,
    seed: int = 42,
) -> Image.Image:
    """Generate a clean top-down prop sprite with transparent background.

    Produces a high-resolution 256px/grid RGBA sprite matching tabletop aesthetics.
    """
    random.seed(seed)
    pw = int(max(64, round(grid_w * 256)))
    ph = int(max(64, round(grid_h * 256)))

    img = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    margin_x = int(pw * 0.1)
    margin_y = int(ph * 0.1)
    bx0, by0 = margin_x, margin_y
    bx1, by1 = pw - margin_x, ph - margin_y

    kind_lower = kind.lower()

    if "banner" in kind_lower or "flag" in kind_lower:
        # Top-down banner / tapestry hanging or laid flat
        # Rich fabric with gold embroidery and pole
        pole_color = (90, 60, 30, 255)
        fabric_color = (140, 25, 35, 255) if "gothic" in style or "crypt" in style else (30, 60, 140, 255)
        trim_color = (210, 175, 55, 255)

        # Pole
        draw.rounded_rectangle([bx0, by0, bx1, by0 + int(ph * 0.15)], radius=4, fill=pole_color, outline=(40, 25, 10, 255), width=2)
        # Finials
        draw.ellipse([bx0 - 4, by0 - 2, bx0 + 6, by0 + int(ph * 0.15) + 2], fill=trim_color)
        draw.ellipse([bx1 - 6, by0 - 2, bx1 + 4, by0 + int(ph * 0.15) + 2], fill=trim_color)
        # Hanging cloth
        cloth_pts = [
            (bx0 + 6, by0 + int(ph * 0.12)),
            (bx1 - 6, by0 + int(ph * 0.12)),
            (bx1 - 6, by1 - int(ph * 0.15)),
            ((bx0 + bx1) // 2, by1),
            (bx0 + 6, by1 - int(ph * 0.15)),
        ]
        draw.polygon(cloth_pts, fill=fabric_color, outline=(20, 20, 20, 255))
        # Gold heraldry / trim
        inner_pts = [
            (bx0 + 14, by0 + int(ph * 0.2)),
            (bx1 - 14, by0 + int(ph * 0.2)),
            (bx1 - 14, by1 - int(ph * 0.22)),
            ((bx0 + bx1) // 2, by1 - int(ph * 0.1)),
            (bx0 + 14, by1 - int(ph * 0.22)),
        ]
        draw.polygon(inner_pts, fill=None, outline=trim_color, width=2)
        # Emblem in centre
        cx, cy = (bx0 + bx1) // 2, (by0 + by1) // 2
        draw.ellipse([cx - 12, cy - 12, cx + 12, cy + 12], fill=trim_color, outline=(60, 40, 10, 255), width=1)

    elif "altar" in kind_lower or "shrine" in kind_lower:
        # Carved stone altar
        stone_base = (70, 70, 75, 255)
        stone_top = (110, 110, 115, 255)
        draw.rounded_rectangle([bx0, by0, bx1, by1], radius=8, fill=stone_base, outline=(30, 30, 35, 255), width=3)
        draw.rounded_rectangle([bx0 + 8, by0 + 8, bx1 - 8, by1 - 8], radius=6, fill=stone_top, outline=(45, 45, 50, 255), width=2)
        # Carved runes / cloth runner
        cx, cy = (bx0 + bx1) // 2, (by0 + by1) // 2
        draw.rectangle([cx - 15, by0 + 4, cx + 15, by1 - 4], fill=(130, 20, 30, 230), outline=(200, 160, 40, 255), width=1)

    elif "rune" in kind_lower or "circle" in kind_lower:
        # Magic circle / glowing runes
        cx, cy = pw // 2, ph // 2
        r = min(pw, ph) // 2 - 10
        glow = (120, 70, 220, 220) if "arcane" in style else (60, 180, 220, 220)
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=glow, width=3)
        draw.ellipse([cx - r + 12, cy - r + 12, cx + r - 12, cy + r - 12], outline=glow, width=2)
        # Inscribed star
        for a in range(0, 360, 72):
            rad = math.radians(a)
            rad2 = math.radians((a + 144) % 360)
            x1, y1 = cx + (r - 16) * math.cos(rad), cy + (r - 16) * math.sin(rad)
            x2, y2 = cx + (r - 16) * math.cos(rad2), cy + (r - 16) * math.sin(rad2)
            draw.line([(x1, y1), (x2, y2)], fill=glow, width=2)

    else:
        # Generic detailed tabletop wooden / stone feature
        draw.rounded_rectangle([bx0, by0, bx1, by1], radius=6, fill=(110, 80, 50, 255), outline=(40, 25, 10, 255), width=3)
        draw.rounded_rectangle([bx0 + 6, by0 + 6, bx1 - 6, by1 - 6], radius=4, fill=(140, 105, 70, 255), outline=(60, 40, 20, 255), width=2)
        draw.line([bx0 + 6, (by0 + by1) // 2, bx1 - 6, (by0 + by1) // 2], fill=(60, 40, 20, 255), width=2)

    # Soft drop shadow underneath
    shadow = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
    sdraw = ImageDraw.Draw(shadow)
    sdraw.rounded_rectangle([bx0 + 4, by0 + 6, bx1 + 4, by1 + 6], radius=6, fill=(0, 0, 0, 100))
    shadow = shadow.filter(ImageFilter.GaussianBlur(radius=4))

    final_img = Image.alpha_composite(shadow, img)
    return final_img


def _comfy_config() -> Dict[str, Any]:
    """The "comfy" block of config.json - checkpoints, sampler, guidance."""
    try:
        config_path = PROJECT_ROOT / "config.json"
        if config_path.exists():
            cfg = json.loads(config_path.read_text(encoding="utf-8"))
            return dict(cfg.get("comfy", {}) or {})
    except Exception:
        pass
    return {}


def comfy_base_url() -> str:
    """Where ComfyUI is, as the user configured it."""
    return str(_comfy_config().get("base_url") or "http://127.0.0.1:8188")


def _style_and_base(style_id: str) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    """The style JSON and the shared base, the same two the map render uses."""
    try:
        from architect import style_data
        style = style_data(style_id) or {}
    except Exception:
        style = {}
    base = {}
    try:
        base_path = PROJECT_ROOT / "styles" / "_base.json"
        if base_path.exists():
            base = json.loads(base_path.read_text(encoding="utf-8"))
    except Exception:
        base = {}
    return style, base


def _prop_aspect(grid_w: float, grid_h: float) -> Tuple[int, int, str]:
    """Canvas for one prop: its own proportions, rounded to what the sampler takes.

    A two-by-one bench rendered on a square canvas comes back square, and gets
    squashed into shape afterwards. Asking for the shape up front is free.
    """
    ratio = max(0.5, min(2.0, float(grid_w or 1) / float(grid_h or 1)))
    side = 1024.0
    width = int(round(side * math.sqrt(ratio) / 16.0)) * 16
    height = int(round(side / math.sqrt(ratio) / 16.0)) * 16
    width = max(512, min(1536, width))
    height = max(512, min(1536, height))
    # The caption says the shape in the plain terms the model reads - "2:1",
    # not the 91:45 that falls out of rounding the canvas to a multiple of 16.
    from fractions import Fraction
    frac = Fraction(ratio).limit_denominator(8)
    return width, height, f"{frac.numerator}:{frac.denominator}"


def make_prop_ideogram_caption(kind: str, style: str = "default", description: str = "",
                               grid_w: float = 1.0, grid_h: float = 1.0) -> str:
    """One prop, in the caption schema Ideogram was trained on.

    The map render sends aspect_ratio, high_level_description, style_description
    and compositional_deconstruction, in that order and with style_description's
    own five keys in theirs. The foundry used to send a shape of its own
    invention - a bare background and elements pair - and got back what you
    would expect from a caption the model has never seen: mush.

    Everything the style knows travels with it too, so a prop for a forest is
    painted in the same hand and the same palette as the forest it will stand
    in.
    """
    st, base = _style_and_base(style)
    name = kind.replace("_", " ").strip() or "object"
    desc = (description or "").strip().rstrip(".")
    _w, _h, aspect = _prop_aspect(grid_w, grid_h)

    head = (f"A single hand-painted top-down fantasy tabletop RPG battle map prop: one {name}, "
            f"seen from directly overhead, alone in the middle of a plain flat grey backdrop")
    if desc:
        head += ", " + desc[0].lower() + desc[1:]
    # Ideogram takes no negative prompt, so what must not be there is stated
    # positively, inside the caption.
    forbidden = (
        "The picture contains exactly one object and nothing else: no floor, no ground, no "
        "grass, no scenery, no second object, no people, creatures or animals, and no text, "
        "letters, numbers, labels, borders or grid lines anywhere")

    caption = {
        "aspect_ratio": aspect,
        "high_level_description": head + ". " + forbidden + ".",
        "style_description": {
            "aesthetics": st.get("aesthetics") or base.get("aesthetics", ""),
            "lighting": ("even flat neutral lighting from directly above, "
                         "a short soft shadow directly beneath the object"),
            "medium": base.get("medium", "Inked line art with watercolour and gouache painting"),
            "art_style": (st.get("art_style") or base.get("art_style",
                          "hand-painted fantasy cartography, inked line art over "
                          "watercolour and gouache, flat orthographic top-down battle map")),
            "color_palette": list(st.get("hex_palette") or base.get("default_palette") or
                                  ["#C8B99A", "#8A7B63", "#4A4038", "#2E2A26", "#6E7A6B"]),
        },
        "compositional_deconstruction": {
            # Not white. A white void gives the model nothing to place the
            # object against and gives the background remover nothing to
            # separate a pale prop from - bones, canvas and weathered stone
            # came back as a blank sheet with a few marks on it. A flat mid
            # grey has contrast in both directions.
            "background": (
                "A completely flat, even, plain mid-grey backdrop filling the whole picture, "
                "with no texture, no gradient, no pattern, no scenery and no objects on it. It "
                "is an empty studio backdrop, not a floor and not ground. The single object sits "
                "in the middle of it with clear empty grey all the way around it."),
            "elements": [
                {
                    # [y1, x1, y2, x2] on a 0-1000 grid, the same convention the
                    # map caption uses. Inset, so the cutout has an edge to find.
                    "box_2d": [150, 150, 850, 850],
                    "label": name,
                    "description": (
                        f"One single {name}"
                        + (f", {desc}" if desc else "")
                        + ", drawn as its top surface exactly as it looks from straight above, "
                          "centred, whole, unobstructed, with crisp clean painted edges and a "
                          "short soft shadow beside it on the grey backdrop"),
                }
            ],
        },
    }
    return json.dumps(caption, ensure_ascii=False, separators=(",", ":"))


# A finished sprite is judged before it is kept, on two numbers measured from
# eight real renders of a forest - four good, four not.
#
#   charred_wooden_tripod    ink  6.1%   biggest blob 100%    good
#   dense_fern_undergrowth   ink 57.5%   biggest blob 100%    good
#   torn_leather_saddle      ink 58.9%   biggest blob 100%    good
#   damp_leaf_litter         ink 81.7%   biggest blob 100%    good
#   moldering_tent_frame     ink  0.0%                        empty sheet
#   scattered_acorns         ink  0.5%   biggest blob  55%    two specks
#   tangled_root_tangle      ink  3.0%   biggest blob  58%    two clumps, far apart
#   muddy_ruts_in_earth      ink  5.0%   biggest blob  63%    three squares and an arrow
#
# Ink alone cannot tell the tripod from the muddy ruts - a tripod is three
# sticks and legitimately thin. What separates them is that a prop is one
# thing: everything worth keeping was a single blob, and everything worth
# throwing away was scattered.
MIN_INK = 0.015
MAX_INK = 0.97
MIN_LARGEST_BLOB = 0.75


def _largest_blob_share(mask) -> float:
    """What fraction of the drawn pixels belong to the single biggest shape.

    Measured on a coarse grid, because the question is whether the picture is
    one object or several, and that does not need full resolution.
    """
    try:
        import numpy as np
    except ImportError:
        return 1.0
    from PIL import Image as _Image

    small = np.asarray(
        _Image.fromarray((mask * 255).astype("uint8")).resize((128, 128), _Image.NEAREST)
    ) > 128
    if not small.any():
        return 0.0

    # Flood fill, iteratively so a long thin shape cannot blow the stack.
    seen = np.zeros_like(small, dtype=bool)
    height, width = small.shape
    best = 0
    total = 0
    for sy, sx in zip(*np.nonzero(small)):
        if seen[sy, sx]:
            continue
        stack = [(int(sy), int(sx))]
        seen[sy, sx] = True
        size = 0
        while stack:
            y, x = stack.pop()
            size += 1
            for ny in range(max(0, y - 1), min(height, y + 2)):
                for nx in range(max(0, x - 1), min(width, x + 2)):
                    if small[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
        total += size
        best = max(best, size)
    return best / float(total) if total else 0.0


def cutout_quality(img: "Image.Image") -> Tuple[bool, str]:
    """(usable, why not) for a finished prop sprite."""
    if img is None:
        return False, "nothing rendered"
    if img.mode != "RGBA":
        return False, "no transparency - the background was never removed"
    try:
        import numpy as np
    except ImportError:
        # Without numpy only the coarse test is possible, which still catches
        # a blank sheet and a solid square.
        alpha = img.getchannel("A")
        ink = sum(c for v, c in enumerate(alpha.histogram()) if v > 128) / float(img.width * img.height)
        blob = 1.0
    else:
        mask = np.asarray(img.getchannel("A")) > 128
        ink = float(mask.mean())
        blob = _largest_blob_share(mask) if mask.any() else 0.0

    if ink < MIN_INK:
        return False, f"almost nothing was drawn ({ink:.1%} of the sprite)"
    if ink > MAX_INK:
        return False, f"the background was not removed ({ink:.1%} of the sprite is opaque)"
    if blob < MIN_LARGEST_BLOB:
        return False, (f"it is not one object - the biggest piece is only {blob:.0%} "
                       f"of what was drawn")
    return True, ""


def _cut_out(raw_img: "Image.Image") -> Optional["Image.Image"]:
    """Remove the background. None when rembg is not installed.

    Returning the picture unchanged - which is what happened here before - saves
    a prop with its white studio background baked in, and a white square is what
    then gets placed on the map.
    """
    try:
        import rembg
    except ImportError:
        return None
    try:
        return rembg.remove(raw_img, alpha_matting=True).convert("RGBA")
    except Exception:
        try:
            return rembg.remove(raw_img).convert("RGBA")
        except Exception:
            return None


def _fit_to_cell(img: "Image.Image", grid_w: float, grid_h: float) -> "Image.Image":
    """Crop to what was drawn and lay it on a transparent canvas of the right size."""
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)
    target_w = int(max(64, round(grid_w * 256)))
    target_h = int(max(64, round(grid_h * 256)))
    cur_w, cur_h = img.size
    scale = min(target_w / cur_w, target_h / cur_h)
    resized = img.resize((max(1, int(cur_w * scale)), max(1, int(cur_h * scale))), Image.LANCZOS)
    canvas = Image.new("RGBA", (target_w, target_h), (0, 0, 0, 0))
    canvas.paste(resized, ((target_w - resized.width) // 2,
                           (target_h - resized.height) // 2), resized)
    return canvas


def _collect_round(client, cfg_prop: Dict[str, Any], jobs: List[Tuple[int, Dict[str, Any]]],
                   seed_shift: int, on_progress=None) -> Dict[int, "Image.Image"]:
    """Queue one round of prop renders, then collect them in order.

    Every prompt goes into ComfyUI's queue up front. The queue runs them one at
    a time either way, but with a single prompt in it the card sat idle through
    the download, the cutout and the write of each prop before the next one was
    even queued.
    """
    from workflow import build_ideogram4
    import tempfile

    results: Dict[int, "Image.Image"] = {}
    queued: List[Tuple[str, int, Dict[str, Any]]] = []
    for index, req in jobs:
        width, height, _aspect = _prop_aspect(req.get("grid_w", 1.0), req.get("grid_h", 1.0))
        caption_json = make_prop_ideogram_caption(
            kind=req.get("kind", "prop"),
            style=req.get("style", "default"),
            description=req.get("description", ""),
            grid_w=req.get("grid_w", 1.0),
            grid_h=req.get("grid_h", 1.0),
        )
        graph = build_ideogram4(cfg_prop, caption_json=caption_json,
                                seed=int(req.get("seed", 42)) + seed_shift,
                                width=width, height=height)
        try:
            queued.append((client.queue_prompt(graph), index, req))
        except Exception as exc:
            print(f"[foundry] Could not queue '{req.get('kind')}': {exc}")

    if not queued:
        return results
    print(f"[foundry] Queued {len(queued)} prop renders in ComfyUI.", flush=True)

    # The queue runs one at a time, so the last prompt waits for all the ones
    # in front of it. A timeout counted from when it was queued has to allow
    # for them, or the tail of a batch times out while still waiting its turn.
    per_prop = 300
    try:
        for position, (prompt_id, index, req) in enumerate(queued):
            kind = req.get("kind", "prop")
            remaining = len(queued) - position
            note = f"[foundry] ({position + 1}/{len(queued)}) {kind}"
            if on_progress:
                on_progress(note)
            else:
                print(note, flush=True)
            try:
                outputs = client.wait(prompt_id, timeout=per_prop * remaining + 120)
            except Exception as exc:
                print(f"[foundry] {kind}: {exc}")
                continue

            with tempfile.TemporaryDirectory() as tmpdir:
                images = client.get_images(outputs, tmpdir)
                if not images:
                    print(f"[foundry] {kind}: ComfyUI returned no image")
                    continue
                raw_img = Image.open(images[0]).convert("RGBA")

            cutout = _cut_out(raw_img)
            if cutout is None:
                print("[foundry] rembg is not installed, so the background cannot be removed. "
                      "Install it with `pip install rembg` - until then props come from the "
                      "procedural generator.")
                break
            # Judged after fitting, not before: the test is about the sprite
            # that will be placed on the map, and a prop measured while still
            # adrift on a big canvas reads as empty however good it is.
            sprite = _fit_to_cell(cutout, req.get("grid_w", 1.0), req.get("grid_h", 1.0))
            usable, why = cutout_quality(sprite)
            if not usable:
                print(f"[foundry] {kind}: discarded, {why}")
                continue
            results[index] = sprite
    except BaseException:
        # A cancelled run must not leave the rest of the batch painting away in
        # the background: whatever has not started yet is dropped from the queue.
        client.cancel([pid for pid, idx, _r in queued if idx not in results])
        raise

    return results


def render_comfy_props(
    requests: List[Dict[str, Any]],
    base_url: Optional[str] = None,
    on_progress=None,
) -> Dict[int, "Image.Image"]:
    """Render several props in one pass, keyed by each request's index.

    A request is a dict of kind, style, description, grid_w, grid_h and seed.
    Anything the first round drew badly enough to be thrown away is asked for
    once more on a different seed before it is given up on - one bad roll is
    not the same as the library being unable to draw the thing.
    """
    results: Dict[int, "Image.Image"] = {}
    if not requests:
        return results
    try:
        from comfy import ComfyClient
        import workflow  # noqa: F401  - imported here so a missing one is caught
    except ImportError as exc:
        print(f"[foundry] ComfyUI support is unavailable ({exc}); using the procedural generator")
        return results

    client = ComfyClient(base_url=base_url or comfy_base_url())
    ok, detail = client.health()
    if not ok:
        print(f"[foundry] ComfyUI is not reachable ({detail}); using the procedural generator")
        return results

    # The card is about to be ComfyUI's. The planner model that wrote the plan
    # is still resident in it - exporting to Dungeondraft never went through
    # the render path that frees it - so free it here, at the one point every
    # foundry entry point passes through.
    try:
        from ollama_client import free_planner_model
        if free_planner_model():
            print("[foundry] Released the planner model from Ollama.")
    except Exception:
        pass

    # The renderer settings live under "comfy", the same place the map render
    # reads them from. This used to look for a top-level "ideogram" key, which
    # no config has ever had, so every prop was drawn by a graph built out of
    # nothing but hardcoded defaults - no checkpoint, sampler or guidance the
    # user had chosen ever reached it.
    # A prop is rendered at whatever quality the map is rendered at. It used to
    # pin its own Turbo and its own twelve steps, so turning the map up to
    # Ultra left every prop a smudge and there was no setting anywhere that
    # said so. The seed is the one thing not taken from here, because each prop
    # carries its own.
    cfg_prop = dict(comfy_cfg := dict(_comfy_config()))
    cfg_prop["ideogram"] = dict(comfy_cfg.get("ideogram", {}) or {})

    jobs = list(enumerate(requests))
    for attempt in range(2):
        if not jobs:
            break
        if attempt:
            print(f"[foundry] Asking again for {len(jobs)} prop(s) on a fresh seed.", flush=True)
        results.update(_collect_round(client, cfg_prop, jobs, seed_shift=attempt * 7919,
                                      on_progress=on_progress))
        jobs = [(i, r) for i, r in jobs if i not in results]

    return results


def render_comfy_prop(
    kind: str,
    style: str = "default",
    description: str = "",
    grid_w: float = 1.0,
    grid_h: float = 1.0,
    seed: int = 42,
    base_url: Optional[str] = None,
) -> Optional["Image.Image"]:
    """One prop through ComfyUI. A batch of one, so both paths behave alike."""
    out = render_comfy_props([{
        "kind": kind, "style": style, "description": description,
        "grid_w": grid_w, "grid_h": grid_h, "seed": seed,
    }], base_url=base_url)
    return out.get(0)


class PropFoundry:
    """Foundry for creating, cataloguing and packaging custom props into Dungeondraft packs."""

    # How many prop renders go into ComfyUI's queue at once.
    BATCH_SIZE = 8

    def __init__(
        self,
        output_dir: Optional[Path] = None,
        db_path: Optional[Path] = None,
    ):
        self.output_dir = output_dir or GENERATED_DIR_DEFAULT
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.db = AssetDatabase(db_path=db_path)
        self._ensure_pack_registered()

    def _ensure_pack_registered(self):
        pack_dict = {
            "id": PACK_ID,
            "name": PACK_NAME,
            "author": PACK_AUTHOR,
            "version": "1.0",
            "file_path": str(self.output_dir.parent / f"{PACK_ID}.dungeondraft_pack"),
            "file_size": 0,
            "file_mtime": int(time.time()),
            "is_builtin": 0,
            "enabled": 1,
            "duplicate_of": None,
            "indexed_at": int(time.time()),
            "scan_version": 1,
        }
        self.db.upsert_pack(pack_dict)

    def close(self):
        self.db.close()

    def generate_prop(
        self,
        kind: str,
        style: str = "default",
        description: str = "",
        grid_w: float = 1.0,
        grid_h: float = 1.0,
        seed: int = 42,
        force_procedural: bool = False,
        prerendered: Optional["Image.Image"] = None,
    ) -> Dict[str, Any]:
        """Generate a single prop, write PNG, and record in generated_props table.

        `prerendered` is an image the caller already got out of ComfyUI, which
        is how a batch queued in one go still ends up recorded one prop at a
        time.
        """
        clean_name = kind.lower().replace(" ", "_")
        clean_style = style.lower().replace(" ", "_")
        prop_id_str = f"{clean_name}_{clean_style}_{grid_w}x{grid_h}_{seed}"
        prop_id = hashlib.sha256(prop_id_str.encode("utf-8")).hexdigest()[:16]

        prop_filename = f"{clean_name}_{prop_id[:8]}.png"
        style_dir = self.output_dir / clean_style
        style_dir.mkdir(parents=True, exist_ok=True)
        png_path = style_dir / prop_filename

        # 1. Try ComfyUI Ideogram + Rembg cutout
        img = prerendered
        backend = "ideogram_comfyui_rembg" if img is not None else "procedural_foundry"
        if img is None and not force_procedural:
            img = render_comfy_prop(
                kind=kind,
                style=style,
                description=description,
                grid_w=grid_w,
                grid_h=grid_h,
                seed=seed,
            )
            if img is not None:
                backend = "ideogram_comfyui_rembg"

        # 2. Fallback to procedural generator
        if img is None:
            img = make_procedural_prop(kind=kind, style=style, grid_w=grid_w, grid_h=grid_h, seed=seed)

        img.save(png_path, "PNG")

        metrics = analyze_image(img)
        thumb_rel = save_thumbnail(img, metrics["content_hash"], self.db.thumbs_dir)

        desc = description or f"A top-down detailed {kind.replace('_', ' ')} suited for {style} battlemaps."

        record = {
            "id": prop_id,
            "name": kind,
            "description": desc,
            "style_id": style,
            "png_path": str(png_path),
            "grid_w": grid_w,
            "grid_h": grid_h,
            "backend": backend,
            "seed": seed,
            "packed_into": PACK_ID,
            "created_at": int(time.time()),
        }
        self.db.upsert_generated_prop(record)

        # Also register directly in assets & enrichment tables so matcher finds it
        res_path = f"res://packs/{PACK_ID}/textures/objects/{clean_style}/{prop_filename}"
        asset_id = compute_asset_id(PACK_ID, res_path)
        asset_dict = {
            "id": asset_id,
            "pack_id": PACK_ID,
            "res_path": res_path,
            "category": "objects",
            "subpath": f"generated/{clean_style}",
            "file_name": prop_filename,
            "width": metrics["width"],
            "height": metrics["height"],
            "grid_w": grid_w,
            "grid_h": grid_h,
            "has_alpha": 1,
            "alpha_coverage": metrics["alpha_coverage"],
            "mean_rgb": metrics["mean_rgb"],
            "palette": metrics["palette"],
            "thumb_path": thumb_rel,
            "pack_tags": json.dumps(["Generated", kind]),
            "pack_sets": json.dumps(["DndBattlemapGenerator"]),
            "content_hash": metrics["content_hash"],
            "state": "ok",
            "last_seen_at": int(time.time()),
        }
        self.db.upsert_asset(asset_dict)

        enr_dict = {
            "content_hash": metrics["content_hash"],
            "description": desc,
            "object_kind": kind.lower().replace("_", " "),
            "semantic_tags": json.dumps(["generated", kind, clean_style]),
            "style_tags": json.dumps([clean_style]),
            "setting_tags": json.dumps(["any"]),
            "dominant_hue": "",
            "confidence": 0.95,
            "model": "foundry",
            "prompt_version": 2,
            "created_at": int(time.time()),
        }
        self.db.upsert_enrichment(enr_dict)

        print(f"Generated prop '{kind}' ({grid_w}x{grid_h} sq) -> {png_path}")
        return record

    def build_custom_pack(self, target_pack_path: Optional[str] = None) -> str:
        """Package all generated props into a native .dungeondraft_pack."""
        config = read_dungeondraft_config()
        cad = config.get("custom_assets_directory")

        if target_pack_path:
            pack_out = Path(target_pack_path)
        elif cad and os.path.exists(cad):
            pack_out = Path(cad) / f"{PACK_ID}.dungeondraft_pack"
        else:
            pack_out = self.output_dir.parent / f"{PACK_ID}.dungeondraft_pack"

        writer = PckWriter(str(pack_out))

        # 1. pack.json
        pack_manifest = {
            "name": PACK_NAME,
            "id": PACK_ID,
            "version": "1.0",
            "author": PACK_AUTHOR,
            "allow_3rd_party_mapping_software_to_read": True,
            "custom_color_overrides": {"enabled": False},
        }
        manifest_bytes = json.dumps(pack_manifest, indent=2).encode("utf-8")
        # Dungeondraft finds a pack by the descriptor sitting directly under
        # res://packs/, not by the one inside the pack's own folder. Every pack
        # that loads has both; ours had only the inner one, so Dungeondraft
        # mounted the archive, found no pack in it, and reported the pack as
        # missing on every map that used it - restarting made no difference
        # because there was nothing to find.
        writer.add_file(f"res://packs/{PACK_ID}.json", manifest_bytes)
        writer.add_file(f"res://packs/{PACK_ID}/pack.json", manifest_bytes)

        # 2. Preview image (256x320)
        preview_img = Image.new("RGBA", (256, 320), (35, 30, 45, 255))
        pdraw = ImageDraw.Draw(preview_img)
        pdraw.rectangle([10, 10, 246, 310], outline=(200, 170, 60, 255), width=2)
        pdraw.text((30, 140), "Custom Props", fill=(240, 240, 240, 255))
        pbuf = io.BytesIO()
        preview_img.save(pbuf, "PNG")
        writer.add_file(f"res://packs/{PACK_ID}/preview.png", pbuf.getvalue())

        # 3. Tags & Textures
        tag_map: Dict[str, List[str]] = {"Generated": []}
        all_pngs = list(self.output_dir.rglob("*.png"))

        for png in all_pngs:
            rel_style = png.parent.name
            tex_rel = f"textures/objects/{rel_style}/{png.name}"
            res_path = f"res://packs/{PACK_ID}/{tex_rel}"
            writer.add_from_disk(res_path, str(png))
            tag_map["Generated"].append(tex_rel)

        tags_data = {
            "tags": tag_map,
            "sets": {"DndBattlemapGenerator": ["Generated"]},
        }
        writer.add_file(f"res://packs/{PACK_ID}/data/default.dungeondraft_tags", json.dumps(tags_data, indent=2).encode("utf-8"))

        writer.write()

        # Update pack row in DB
        stat = os.stat(str(pack_out))
        pack_dict = {
            "id": PACK_ID,
            "name": PACK_NAME,
            "author": PACK_AUTHOR,
            "version": "1.0",
            "file_path": os.path.abspath(str(pack_out)),
            "file_size": stat.st_size,
            "file_mtime": int(stat.st_mtime),
            "is_builtin": 0,
            "enabled": 1,
            "duplicate_of": None,
            "indexed_at": int(time.time()),
            "scan_version": 1,
        }
        self.db.upsert_pack(pack_dict)

        # Enable pack in Dungeondraft's config.ini so Dungeondraft loads it without 'missing pack' caution
        listed = enable_pack_in_dungeondraft_config(PACK_ID)

        print(f"Packed {len(all_pngs)} textures into Dungeondraft pack: {pack_out}")
        # Dungeondraft reads its asset packs once, at startup, and writes its
        # own settings back out when it closes. A pack built or enabled while
        # it is open is therefore invisible to it - and the map that uses the
        # pack opens with "This map was created with the following missing
        # packs loaded". Saying so here is the difference between a warning
        # that explains itself and one that looks like a broken export.
        if listed:
            print("[foundry] Close Dungeondraft and open it again before loading a map that "
                  "uses these props. It reads its asset packs only when it starts, so a pack "
                  "added while it is running shows up as a missing pack.")
        else:
            print("[foundry] Could not add the pack to Dungeondraft's active_asset_packs. "
                  f"Enable '{PACK_NAME}' by hand in Dungeondraft's asset pack list, then "
                  "restart it.")
        return str(pack_out)

    def prune_bad_props(self) -> int:
        """Throw away generated props that would not pass the quality check now.

        The check arrived after props had already been made and packed, and a
        prop that is a blank sheet or three unrelated specks is worse than no
        prop at all: it is placed on the map and looks like a bug. Removing one
        puts its kind back in front of the foundry on the next export.
        """
        cur = self.db.conn.cursor()
        rows = [dict(r) for r in cur.execute(
            "SELECT id, name, png_path FROM generated_props").fetchall()]
        removed = 0
        for row in rows:
            png = Path(row["png_path"])
            if not png.exists():
                continue
            try:
                usable, why = cutout_quality(Image.open(png).convert("RGBA"))
            except Exception as exc:
                usable, why = False, f"could not be read ({exc})"
            if usable:
                continue
            print(f"[foundry] dropping {row['name']}: {why}")
            res_path = None
            hit = cur.execute(
                "SELECT res_path, content_hash FROM assets WHERE pack_id = ? AND file_name = ?",
                (PACK_ID, png.name)).fetchone()
            if hit:
                res_path, content_hash = hit[0], hit[1]
                cur.execute("DELETE FROM assets WHERE res_path = ?", (res_path,))
                cur.execute("DELETE FROM enrichment WHERE content_hash = ?", (content_hash,))
            cur.execute("DELETE FROM generated_props WHERE id = ?", (row["id"],))
            png.unlink()
            removed += 1
        self.db.conn.commit()
        if removed:
            self.build_custom_pack()
        return removed

    def satisfy_unmatched(self, unmatched_list: List[dict], style: str = "default",
                          seed: int = 42) -> int:
        """Generate and pack all unmatched props from a report.

        One prop kind is rendered once however many times the map wants it, and
        the whole set goes into ComfyUI's queue in one go rather than one at a
        time with the card idle in between. The footprint and the description
        the plan gave each prop travel with it - a two-square fallen log used to
        be asked for as a one-square nothing, because a bare kind name was all
        this was ever handed.
        """
        requests: List[Dict[str, Any]] = []
        seen: Set[str] = set()
        for item in unmatched_list:
            kind = str(item.get("kind", "prop"))
            if kind in seen:
                continue
            seen.add(kind)
            requests.append({
                "kind": kind,
                "style": style,
                "description": str(item.get("description") or ""),
                "grid_w": max(0.5, float(item.get("w") or 1.0)),
                "grid_h": max(0.5, float(item.get("h") or 1.0)),
                "seed": seed + (hash(kind) & 0xFFFF),
            })
        if not requests:
            return 0

        created = 0
        drawn = 0
        # Queued a batch at a time rather than all at once. Two reasons: a
        # shared ComfyUI is not monopolised by one export, and a run stopped
        # from the app - which kills this process outright, with no chance to
        # tidy up - leaves at most a batch still painting instead of the whole
        # list. Each batch is written to the database before the next is
        # queued, so an interrupted run keeps what it already made.
        for start in range(0, len(requests), self.BATCH_SIZE):
            chunk = requests[start:start + self.BATCH_SIZE]
            rendered = render_comfy_props(chunk)
            for index, req in enumerate(chunk):
                self.generate_prop(
                    kind=req["kind"],
                    style=req["style"],
                    description=req["description"],
                    grid_w=req["grid_w"],
                    grid_h=req["grid_h"],
                    seed=req["seed"],
                    prerendered=rendered.get(index),
                )
                created += 1
            drawn += len(rendered)
        print(f"[foundry] {drawn} of {created} props came from Ideogram; "
              f"{created - drawn} fell back to the procedural generator.")
        if created > 0:
            self.build_custom_pack()
        return created


def main():
    parser = argparse.ArgumentParser(description="Dungeondraft Prop Foundry")
    subparsers = parser.add_subparsers(dest="command", help="Command to run")

    gen_parser = subparsers.add_parser("generate", help="Generate a single prop")
    gen_parser.add_argument("--prop", type=str, required=True, help="Prop kind/name (e.g. banner, altar)")
    gen_parser.add_argument("--style", type=str, default="default", help="Map style ID")
    gen_parser.add_argument("--width", type=float, default=1.0, help="Width in grid cells")
    gen_parser.add_argument("--height", type=float, default=1.0, help="Height in grid cells")
    gen_parser.add_argument("--seed", type=int, default=42, help="Random seed")

    pack_parser = subparsers.add_parser("pack", help="Build .dungeondraft_pack from all generated props")
    pack_parser.add_argument("--out", type=str, help="Output .dungeondraft_pack path")

    batch_parser = subparsers.add_parser("satisfy", help="Satisfy unmatched props from a .report.json file")
    batch_parser.add_argument("report_file", type=str, help="Path to .report.json")

    subparsers.add_parser("prune", help="Delete generated props that fail the quality check")

    args = parser.parse_args()
    foundry = PropFoundry()

    try:
        if args.command == "generate":
            foundry.generate_prop(
                kind=args.prop,
                style=args.style,
                grid_w=args.width,
                grid_h=args.height,
                seed=args.seed,
            )
            foundry.build_custom_pack()
        elif args.command == "pack":
            foundry.build_custom_pack(target_pack_path=getattr(args, "out", None))
        elif args.command == "satisfy":
            report_path = Path(args.report_file)
            if not report_path.exists():
                sys.exit(f"Report file not found: {report_path}")
            data = json.loads(report_path.read_text(encoding="utf-8"))
            unmatched = data.get("unmatched_props", [])
            style = data.get("style", "default")
            count = foundry.satisfy_unmatched(unmatched, style=style)
            print(f"Satisfied {count} unmatched props for {report_path.name}.")
        elif args.command == "prune":
            gone = foundry.prune_bad_props()
            print(f"Removed {gone} unusable generated prop(s). "
                  "Export again to have them rendered afresh.")
        else:
            parser.print_help()
    finally:
        foundry.close()


if __name__ == "__main__":
    main()
