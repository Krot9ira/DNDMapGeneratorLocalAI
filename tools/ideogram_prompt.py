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
MAX_ELEMENTS = 60
# Walls are merged into the longest runs the grid allows, so this is generous.
MAX_WALL_RUNS = 12

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


def _tile_rect(x, y, w, h, cols, rows, limit=0.22, max_tiles=6):
    """Split a rectangle into tiles when it covers a big share of the map.

    A single bounding box the size of half the frame is not a useful
    instruction - the model satisfies it with one blob somewhere inside. Several
    adjacent boxes carrying the same description force the whole area to be
    covered.
    """
    if cols <= 0 or rows <= 0:
        return [(x, y, w, h)]
    if (w * h) <= limit * cols * rows:
        return [(x, y, w, h)]
    nx = 3 if w >= cols * 0.6 else 2
    ny = 3 if h >= rows * 0.6 else 2
    while nx * ny > max_tiles:
        if nx >= ny:
            nx -= 1
        else:
            ny -= 1
    nx, ny = max(1, nx), max(1, ny)
    out = []
    for j in range(ny):
        for i in range(nx):
            tx = x + w * i // nx
            ty = y + h * j // ny
            tw = (x + w * (i + 1) // nx) - tx
            th = (y + h * (j + 1) // ny) - ty
            if tw > 0 and th > 0:
                out.append((tx, ty, tw, th))
    return out or [(x, y, w, h)]


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

    # Counted openings. Without this the renderer decorates a long wall with a
    # row of invented archways, which changes how the map plays.
    door_count = sum(w * h for (x, y, w, h) in _merge_runs(grid, A.DOOR))
    window_count = sum(w * h for (x, y, w, h) in _merge_runs(grid, A.WINDOW))
    opening_note = (
        f"Every wall in this map is solid from end to end. There are exactly {door_count} "
        f"doors and {window_count} windows in the whole picture, each one listed below with "
        f"its own rectangle, and no other door, doorway, archway, gate, gap or window exists "
        f"anywhere in any wall.")
    caption["high_level_description"] += " " + opening_note
    caption["high_level_description"] += (
        " The buildings are of different sizes and stand in an irregular arrangement; the "
        "layout is not symmetrical, not mirrored and not a repeating pattern.")

    caption["style_description"] = {
        "aesthetics": style.get("aesthetics") or base.get("aesthetics", ""),
        "lighting": style.get("lighting") or base.get("lighting", ""),
        "medium": base.get("medium", "Inked line art with watercolour and gouache painting"),
        "color_palette": list(style.get("hex_palette") or base.get("default_palette") or
                              ["#C8B99A", "#8A7B63", "#4A4038", "#2E2A26", "#6E7A6B"]),
    }

    # Elements are gathered by priority, because the cap must never drop the
    # things whose position actually matters.
    # Four tiers. `structure` is what the map is: walls, doors, windows, the
    # ship. It outranks rooms and scenery because a battle map with the wrong
    # walls is the wrong map, however well painted.
    critical, walls, structure, normal, filler = [], [], [], [], []

    # 0. The blank margin. Said in the background too, but a sentence is weaker
    #    than a box: with a full-map effect the renderer painted straight over
    #    it. These four go first so nothing outranks them.
    border = A.border_of(map_data)
    if border > 0:
        margin_text = ("Flat empty unpainted margin filling this whole strip, the blank "
                       "paper border outside the map: no ground texture, no scenery, no "
                       "building, no water, no prop, no smoke and no effect of any kind "
                       "reaches into it")
        for rect in ((0, 0, cols, border),
                     (0, rows - border, cols, border),
                     (0, border, border, rows - 2 * border),
                     (cols - border, border, border, rows - 2 * border)):
            critical.append({"type": "obj",
                             "bbox": _bbox(rect[0], rect[1], rect[2], rect[3], cols, rows),
                             "desc": margin_text + ". " + _EXACT})

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
        ex, ey = int(eff.get("x", 0)), int(eff.get("y", 0))
        ew, eh = max(1, int(eff.get("w", 1))), max(1, int(eff.get("h", 1)))
        body_text = (f"{text}. This is an atmospheric effect painted over the scene, {how}, "
                     f"lying on top of the ground without replacing it")
        # One huge box makes the model paint the effect in a single corner and
        # call it done. Splitting a large area into tiles forces the coverage it
        # was asked for, because every tile has to be filled on its own.
        for (tx, ty, tw, th) in _tile_rect(ex, ey, ew, eh, cols, rows):
            spread = ("" if (tw, th) == (ew, eh) else
                      ", and this patch of it is one part of a single continuous effect "
                      "that covers the whole marked region")
            critical.append({
                "type": "obj",
                "bbox": _bbox(tx, ty, tw, th, cols, rows),
                "desc": f"{body_text}{spread}. It fills this whole rectangle. {_EXACT}"})

    # 3. Structures.
    for st in map_data.get("structures", []) or []:
        if st.get("kind") != "ship":
            continue
        structure.append({
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
    #    (see below - walls are emitted first, because openings only make sense
    #    once the runs they sit in exist)
    #    gets its own box - without this the renderer put openings where it liked.
    door_word = style.get("door") or "a heavy closed timber door with iron banding and a ring handle"
    for (x, y, w, h) in _merge_runs(grid, A.DOOR):
        structure.append({"type": "obj", "bbox": _bbox(x, y, w, h, cols, rows),
                         "desc": f"{door_word}, set into the wall and completely filling the "
                                 f"opening as a solid closed door leaf, not an empty gap. "
                                 f"{_EXACT}"})

    # 4b. Windows. Like doors, few and load-bearing: they say where the wall is
    #     broken by an opening, and the renderer will invent them anywhere if it
    #     is not told exactly where they belong.
    window_word = style.get("window") or (
        "a window set into the wall: a stone or timber frame holding small panes of "
        "glass, its sill and lintel clearly drawn")
    for (x, y, w, h) in _merge_runs(grid, A.WINDOW):
        structure.append({"type": "obj", "bbox": _bbox(x, y, w, h, cols, rows),
                         "desc": f"{window_word}, filling the whole opening in the wall "
                                 f"and set flush into it, with solid wall continuing on "
                                 f"both sides. {_EXACT}"})

    # 4c. Walls. Until now the renderer was handed room rectangles and left to
    #     invent every wall line itself, which is exactly where the layout came
    #     apart. The architect knows each run, so each run is given.
    # Caves and woodland have no straight walls to speak of; calling their rock
    # a "straight unbroken run" would be a lie the renderer would try to obey.
    organic = str(meta.get("layout", "")).lower() in ("cavern", "forest", "swamp")
    # Merge each logical wall into one rectangle by treating its openings as
    # wall while the runs are found. Handing over the same wall as four separate
    # boxes reads as a repeating pattern, and the renderer answers a repeating
    # pattern with a symmetrical building it invented itself.
    solid = A.TileGrid(grid.cols, grid.rows, A.VOID)
    for yy in range(grid.rows):
        for xx in range(grid.cols):
            k = grid.get(xx, yy)
            solid.set(xx, yy, A.WALL if k in (A.WALL, A.DOOR, A.WINDOW) else k)
    wall_word = style.get("wall") or (
        "solid rough rock wall" if organic else "solid stone wall with visible courses")
    for (x, y, w, h) in _merge_runs(solid, A.WALL)[:MAX_WALL_RUNS]:
        # Only genuinely elongated runs. Everything else is a stub or, in a cave,
        # one lump of an irregular mass, and describing it as a wall adds noise.
        if max(w, h) < 3 or w * h < 2:
            continue
        if organic:
            walls.append({
                "type": "obj",
                "bbox": _bbox(x, y, w, h, cols, rows),
                "desc": (f"A mass of {wall_word} filling this whole rectangle solidly, its "
                         f"face irregular and broken but with no passage, gap or opening "
                         f"through it anywhere. {_EXACT}")})
            continue
        if w >= h:
            shape = ("a horizontal wall running the full width of this rectangle from its "
                     "left edge to its right edge")
        else:
            shape = ("a vertical wall running the full height of this rectangle from its "
                     "top edge to its bottom edge")
        walls.append({
            "type": "obj",
            "bbox": _bbox(x, y, w, h, cols, rows),
            "desc": (f"A run of {wall_word}: {shape}, filling it completely and keeping the "
                     f"same thickness along its entire length, with square ends and solid "
                     f"masonry everywhere except at the doors listed separately below. "
                     f"{_EXACT}")})

    # 4d. The open ground. Without it the renderer treats every empty square as
    #     somewhere a building could go, and fills the map with invented rooms.
    open_word = style.get("ground") or "open paved ground"
    for (x, y, w, h) in _merge_runs(grid, A.FLOOR)[:3]:
        if w * h < cols * rows * 0.05:
            break
        walls.append({
            "type": "obj",
            "bbox": _bbox(x, y, w, h, cols, rows),
            "desc": (f"Open ground of {open_word} filling this whole rectangle, unbroken "
                     f"from edge to edge: no building, no wall and no partition stands "
                     f"anywhere inside it, only loose objects lying on the ground. "
                     f"{_EXACT}")})

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
            "desc": (f"The {label}: the roofless interior of a room seen from directly "
                     f"above, its floor and furniture fully visible and filling this "
                     f"rectangle, with no roof, no ceiling and nothing overhanging it"),
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

    elements = critical + walls + structure + normal + filler
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
        "painted with visible brushwork, every part of the ground fully painted with no "
        "blank patches inside the map area")
    background = f"{ground} {suffix}"

    # The blank ring around the playable field. Content boxes are already inset
    # by it, but the model still has to be told the margin is meant to be empty,
    # or it fills the space with invented scenery.
    if border > 0:
        note = base.get("border_note") or (
            "A plain flat unpainted margin runs right around the outside of the image on "
            "all four sides, empty of scenery, buildings, water and props, exactly like the "
            "blank paper border of a printed battle map sheet")
        pct_x = max(1, round(border / cols * 100))
        pct_y = max(1, round(border / rows * 100))
        background += (f". {note}. The margin is {pct_x} percent of the image width down each "
                       f"side and {pct_y} percent of its height along the top and bottom")

    caption["compositional_deconstruction"] = {
        "background": background,
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
