#!/usr/bin/env python3
"""
Build an Ideogram 4 structured JSON caption straight from map geometry.

Ideogram 4 is trained exclusively on structured JSON captions, and its bounding
boxes are a real, trained spatial interface rather than decorative text. That is
an unusually good match for this project: the architect already knows the exact
rectangle of every room, wall, water body, ship and prop, so the layout can be
handed over as coordinates instead of being squeezed through a ControlNet.

Contract (from the model's own captioner spec):
  {"aspect_ratio":"W:H",
   "high_level_description":"...",
   "style_description":{...},
   "compositional_deconstruction":{"background":"...","elements":[...]}}

  element = {"type":"obj","bbox":[y1,x1,y2,x2],"desc":"..."}
  bbox is normalised 0-1000 on BOTH axes, top-left origin, y1<y2 and x1<x2.

The spec bans hedging ("various", "such as", "or similar") and demands one
committed value per property, so every string built here is concrete.
"""
import json
from math import gcd

import architect as A

# Cap so no single caption drowns the model in elements.
MAX_ELEMENTS = 40

_TERRAIN_WORDS = {
    A.WATER: "dark green water",
    A.PIT: "an open pit dropping into darkness",
    A.RUBBLE: "loose rubble and broken stone",
    A.VEGETATION: "dense undergrowth",
}

_PROP_WORDS = {
    "pillar": "a thick round stone pillar",
    "column": "a fluted stone column",
    "statue": "a weathered stone statue on a square plinth",
    "altar": "a carved stone altar block",
    "sarcophagus": "a heavy stone sarcophagus with a chipped lid",
    "coffin": "a plain timber coffin",
    "table": "a long timber table",
    "desk": "a writing desk covered in papers",
    "workbench": "a scarred wooden workbench",
    "bench": "a low timber bench",
    "bed": "a straw mattress on a timber frame",
    "throne": "a high-backed carved throne",
    "bookshelf": "a tall shelf packed with ledgers",
    "bar": "a polished timber bar counter",
    "anvil": "a black iron anvil on a stump",
    "forge": "a stone forge glowing with coals",
    "hearth": "a stone hearth with burning logs",
    "brazier": "an iron brazier holding live coals",
    "campfire": "a ring of stones around a burning campfire",
    "cauldron": "a black iron cauldron on a tripod",
    "well": "a round stone well with a timber winch",
    "fountain": "a carved stone fountain basin",
    "tree": "a broad tree seen from above, its canopy spreading wide",
    "boulder": "a moss-covered boulder",
    "stalagmite": "a jagged rock spire",
    "crystal": "a cluster of glowing crystal shards",
    "mast": "the base of a timber mast with coiled rigging",
    "capstan": "a timber capstan with protruding bars",
    "cart": "a two-wheeled timber handcart",
    "wagon": "a four-wheeled timber wagon",
    "dumpster": "a dented steel dumpster",
    "portal": "a standing stone archway",
    "gate": "a heavy iron-bound gate",
    "arch": "a carved stone arch",
}


# How much licence the renderer gets with a custom prop's description.
# Repeated on everything whose position is load-bearing.
_EXACT = "It sits exactly inside this rectangle and nowhere else"

_ELABORATION = {
    "exact": "Render this exactly as described and add nothing to it",
    "some": "Render this as described, filling in fitting detail",
    "free": "Take this as a starting point and elaborate it richly with fitting detail",
}


def _aspect_ratio(cols, rows):
    g = gcd(cols, rows) or 1
    return f"{cols // g}:{rows // g}"


def _bbox(x, y, w, h, cols, rows):
    """Grid rectangle -> [y1, x1, y2, x2] normalised to 0-1000 on both axes."""
    return [
        max(0, min(1000, round(y / rows * 1000))),
        max(0, min(1000, round(x / cols * 1000))),
        max(0, min(1000, round((y + h) / rows * 1000))),
        max(0, min(1000, round((x + w) / cols * 1000))),
    ]


def _merge_runs(grid, kind):
    """Greedy rectangles for one tile kind, largest first."""
    claimed = [[False] * grid.cols for _ in range(grid.rows)]
    rects = []
    for y in range(grid.rows):
        for x in range(grid.cols):
            if claimed[y][x] or grid.get(x, y) != kind:
                continue
            w = 0
            while x + w < grid.cols and not claimed[y][x + w] and grid.get(x + w, y) == kind:
                w += 1
            h = 1
            while y + h < grid.rows and all(
                    not claimed[y + h][x + i] and grid.get(x + i, y + h) == kind
                    for i in range(w)):
                h += 1
            for yy in range(y, y + h):
                for xx in range(x, x + w):
                    claimed[yy][xx] = True
            rects.append((x, y, w, h))
    rects.sort(key=lambda r: r[2] * r[3], reverse=True)
    return rects


def build_caption(map_data, style=None, base=None):
    """Return the structured caption as a dict."""
    grid = A.zones_to_grid(map_data)
    cols, rows = grid.cols, grid.rows
    meta = map_data.get("meta", {}) or {}
    style = style or {}
    base = base or {}

    caption = {"aspect_ratio": _aspect_ratio(cols, rows)}

    title = meta.get("title") or "battle map"
    summary = (meta.get("scene_summary") or "").strip().rstrip(".")
    # Hard 50-word cap, one sentence, no "this image shows" opener.
    head = (f"A hand-painted top-down fantasy tabletop RPG battle map of {title}, "
            f"seen from directly overhead")
    if summary:
        head += ", " + summary[0].lower() + summary[1:]
    words = head.split()
    if len(words) > 44:
        head = " ".join(words[:44])
    # Ideogram takes no negative prompt, so the ban on text and creatures has to
    # be stated positively, inside the caption.
    forbidden = base.get("forbidden_suffix") or (
        "The scene is completely empty of people, creatures and animals, and carries no text, "
        "letters, numbers, labels or grid lines anywhere")
    caption["high_level_description"] = head.rstrip(",") + ". " + forbidden + "."

    caption["style_description"] = {
        "aesthetics": style.get("aesthetics") or base.get("aesthetics", ""),
        "lighting": style.get("lighting") or base.get("lighting", ""),
        "medium": base.get("medium", "Inked line art with watercolour and gouache painting"),
        "color_palette": list(style.get("hex_palette") or base.get("default_palette") or
                              ["#C8B99A", "#8A7B63", "#4A4038", "#2E2A26", "#6E7A6B"]),
    }

    # Elements are gathered by priority, because the cap must never drop the
    # things whose position actually matters.
    critical, normal, filler = [], [], []

    # 1. User-written annotations win over everything: they were placed by hand.
    for ann in map_data.get("annotations", []) or []:
        label = str(ann.get("label", "")).strip()
        if not label:
            continue
        text = label
        detail = str(ann.get("description", "")).strip()
        if detail:
            text += ". " + detail.rstrip(".")
        text += ". " + _ELABORATION.get(str(ann.get("elaboration", "some")).lower(),
                                        _ELABORATION["some"])
        critical.append({"type": "obj",
                         "bbox": _bbox(int(ann.get("x", 0)), int(ann.get("y", 0)),
                                       max(1, int(ann.get("w", 1))),
                                       max(1, int(ann.get("h", 1))), cols, rows),
                         "desc": text + ". " + _EXACT})

    # 2. Effects sit on top of everything, so they are described as overlays and
    #    must never be mistaken for a change of ground material.
    strength = {"low": "faint and thin", "medium": "clearly visible",
                "high": "thick and dominating the area"}
    for eff in map_data.get("effects", []) or []:
        kind = str(eff.get("kind", "")).lower()
        label = str(eff.get("label", "")).strip()
        body = A.EFFECTS.get(kind, "")
        if label:
            text = label
            detail = str(eff.get("description", "")).strip()
            if detail:
                text += ". " + detail.rstrip(".")
            text += ". " + _ELABORATION.get(str(eff.get("elaboration", "some")).lower(),
                                            _ELABORATION["some"])
        elif body:
            text = body[0].upper() + body[1:]
        else:
            continue
        how = strength.get(str(eff.get("intensity", "medium")).lower(), strength["medium"])
        critical.append({
            "type": "obj",
            "bbox": _bbox(int(eff.get("x", 0)), int(eff.get("y", 0)),
                          max(1, int(eff.get("w", 1))), max(1, int(eff.get("h", 1))),
                          cols, rows),
            "desc": (f"{text}. This is an atmospheric effect painted over the scene, {how}, "
                     f"lying on top of the ground without replacing it. {_EXACT}")})

    # 3. Structures.
    for st in map_data.get("structures", []) or []:
        if st.get("kind") != "ship":
            continue
        critical.append({
            "type": "obj",
            "bbox": _bbox(st["x"], st["y"], st["w"], st["h"], cols, rows),
            "desc": ("One large wooden sailing ship floating on the water, viewed from directly "
                     "above so only its weather deck is visible: a pointed bow at the right, a "
                     "raised quarterdeck at the left, a continuous timber bulwark rail running "
                     "round the hull, weathered oak deck planking running fore and aft, a square "
                     "cargo hatch amidships, the round base of a single mast with coiled rigging, "
                     "and mooring ropes running to the dock. No sails and no lower decks."),
        })

    # 4. Doors. Few in number and load-bearing for how the map plays, so each one
    #    gets its own box - without this the renderer put openings where it liked.
    door_word = style.get("door") or "a heavy closed timber door with iron banding and a ring handle"
    for (x, y, w, h) in _merge_runs(grid, A.DOOR):
        critical.append({"type": "obj", "bbox": _bbox(x, y, w, h, cols, rows),
                         "desc": f"{door_word}, set into the wall and completely filling the "
                                 f"opening as a solid closed door leaf, not an empty gap. "
                                 f"{_EXACT}"})

    # 5. Terrain bodies large enough to matter, biggest first.
    for kind, phrase in _TERRAIN_WORDS.items():
        for (x, y, w, h) in _merge_runs(grid, kind)[:2]:
            if w * h < max(4, cols * rows * 0.02):
                continue
            normal.append({"type": "obj", "bbox": _bbox(x, y, w, h, cols, rows),
                           "desc": f"A body of {phrase} filling this region, its edge meeting "
                                   f"the surrounding ground in a clean line. {_EXACT}"})

    # 6. Rooms.
    for area in map_data.get("areas", []) or []:
        label = (area.get("label") or "").strip()
        if not label or label.lower() in ("moored ship", "quay"):
            continue
        normal.append({
            "type": "obj",
            "bbox": _bbox(area["x"], area["y"], area["w"], area["h"], cols, rows),
            "desc": (f"The {label}: a roofless interior seen from above, its floor and "
                     f"furniture fully visible, enclosed by thick stone walls that follow "
                     f"this rectangle exactly"),
        })

    # 7. Load-bearing props get their own box; clutter is described without one
    #    so the renderer can place it naturally.
    loose = []
    for f in map_data.get("features", []) or []:
        kind = str(f.get("kind", "")).lower()
        if not f.get("structural", True):
            loose.append(kind.replace("_", " "))
            continue
        label = str(f.get("label", "")).strip()
        if label:
            text = label
            detail = str(f.get("description", "")).strip()
            if detail:
                text += ". " + detail.rstrip(".")
            text += ". " + _ELABORATION.get(str(f.get("elaboration", "some")).lower(),
                                            _ELABORATION["some"])
            critical.append({"type": "obj",
                             "bbox": _bbox(f["x"], f["y"], 1, 1, cols, rows),
                             "desc": text + ". " + _EXACT})
            continue
        phrase = _PROP_WORDS.get(kind)
        if not phrase:
            continue
        filler.append({"type": "obj",
                       "bbox": _bbox(f["x"], f["y"], 1, 1, cols, rows),
                       "desc": phrase + ", seen from directly above"})

    elements = critical + normal + filler
    if len(elements) > MAX_ELEMENTS:
        elements = elements[:MAX_ELEMENTS]

    if loose:
        counts = {}
        for k in loose:
            counts[k] = counts.get(k, 0) + 1
        listed = ", ".join(f"{n} {k}" + ("s" if n > 1 and not k.endswith("s") else "")
                           for k, n in sorted(counts.items(), key=lambda kv: -kv[1])[:6])
        elements.append({"type": "obj",
                         "desc": f"Scattered clutter across the walkable ground: {listed}, "
                                 f"arranged against walls and in corners, casting soft shadows"})

    ground = style.get("ground") or "worn stone paving"
    # The style's material description used to be dropped entirely; it is the
    # richest source of theme detail we have, so it belongs in the background.
    materials = (style.get("materials") or "").strip()
    if materials:
        ground = f"{ground}. {materials.rstrip('.')}"
    suffix = base.get("background_suffix") or (
        "covering the whole frame, painted edge to edge with visible brushwork; the map fills "
        "the entire image with no border and no blank areas")
    caption["compositional_deconstruction"] = {
        "background": f"{ground} {suffix}",
        "elements": elements,
    }
    return caption


def build_caption_json(map_data, style=None, base=None):
    """Minified single-line JSON, exactly as the model expects."""
    return json.dumps(build_caption(map_data, style, base),
                      ensure_ascii=False, separators=(",", ":"))


if __name__ == "__main__":
    import sys
    from planner import MapPlanner

    src = sys.argv[1] if len(sys.argv) > 1 else "output/bg_docks2/map.json"
    data = json.loads(open(src, encoding="utf-8").read())
    planner = MapPlanner()
    st = planner.load_style(data.get("meta", {}).get("style"))
    print(json.dumps(build_caption(data, st, planner.load_base()),
                     indent=2, ensure_ascii=False))
