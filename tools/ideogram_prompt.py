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
MAX_ELEMENTS = 24               # a one-room scene; see element_budget()
MAX_ELEMENTS_CEILING = 40       # past this they drown each other out whatever they say


def element_budget(map_data):
    """How many elements this map can carry without them blurring together."""
    rooms = sum(1 for a in (map_data.get("areas") or [])
                if str(a.get("label", "")).strip())
    return max(MAX_ELEMENTS, min(MAX_ELEMENTS_CEILING, 16 + 3 * rooms))
# Walls are merged into the longest runs the grid allows, so this is generous.
MAX_WALL_RUNS = 8

# How many of one kind of *filler* object get their own rectangle before the
# rest join the clutter sentence. Filler is what the architect sprinkles in to
# fill a floor; past a few, identical elements stop saying "there are several of
# these" and start saying "this map is a repeating pattern", which the renderer
# obliges by mirroring the layout. Props that were actually asked for are never
# folded away - a room with twelve chests in it is a plan, not noise.
MAX_SAME_PROP = 3

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

# Built-in fallbacks. The live text comes from styles/_phrases.json.
_DOOR_TEXT = (
    "A closed door filling this opening in the wall, seen from directly overhead so only the "
    "flat top face of the door leaf is visible: a plain rectangular timber panel with dark "
    "iron bands across it, lying flush inside the wall opening and squared up with the wall, "
    "exactly as wide as the wall is thick. It is drawn flat, straight on and level with "
    "everything around it, with no perspective, no tilt, no arch or vault above it, no door "
    "frame standing proud, no steps and no visible handle")
_WINDOW_TEXT = (
    "a window set into the wall: a stone or timber frame holding small panes of glass, its "
    "sill and lintel clearly drawn")
_WALL_TEXT = "solid stone wall with visible courses"
_WALL_ORGANIC_TEXT = "solid rough rock wall"
_SHIP_TEXT = (
    "One large wooden sailing ship floating on the water, viewed from directly above so only "
    "its weather deck is visible: a pointed bow at the right, a raised quarterdeck at the "
    "left, a continuous timber bulwark rail running round the hull, weathered oak deck "
    "planking running fore and aft, a square cargo hatch amidships, the round base of a "
    "single mast with coiled rigging, and mooring ropes running to the dock. No sails and no "
    "lower decks.")

# What the outer edge of a site is made of, in words. architect.enclosure_of
# picks the kind; a style can override any of these three for itself with a
# "wall", "face" or "boundary" field of its own.
_ENCLOSURE_WORDS = {
    "masonry": {
        "wall": "solid stone wall with visible courses",
        "face": "plain masonry",
        "boundary": ("a thick stone wall of plain masonry closing the site in on all four "
                     "sides"),
    },
    "rock": {
        "wall": "solid rough rock wall",
        "face": "raw rock",
        "boundary": ("sheer natural rock closing the site in on all four sides, its face "
                     "broken and irregular, and nowhere a course of laid stone"),
    },
    "timber": {
        "wall": "solid timber bulwark of close-fitted planking",
        "face": "close-fitted planking",
        "boundary": ("a continuous timber bulwark of close-fitted planking running right "
                     "round the outside"),
    },
    "open": {
        "wall": "solid rough rock wall",
        "face": "raw rock",
        "boundary": ("a continuous natural edge closing the site in on all four sides - "
                     "rising ground, rock, earth and dense growth, whatever the surroundings "
                     "are - every part of it natural and unbuilt, with no course of laid "
                     "stone, no brickwork and no doorway anywhere along it"),
    },
}

_ELABORATION = {
    "exact": "Render this exactly as described and add nothing to it",
    "some": "Render this as described, filling in fitting detail",
    "free": "Take this as a starting point and elaborate it richly with fitting detail",
}


# The wording for every kind of object lives in styles/_phrases.json so it can
# be edited without touching code. The constants above are the fallback for when
# that file is missing or a key has been deleted from it.
_PHRASES_CACHE = None


def load_phrases():
    """Editable caption wording, merged over the built-in defaults."""
    global _PHRASES_CACHE
    if _PHRASES_CACHE is not None:
        return _PHRASES_CACHE
    data = {}
    try:
        from paths import ROOT as _ROOT
        path = _ROOT / "styles" / "_phrases.json"
        if path.exists():
            data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        data = {}
    merged = {
        "structure": {
            "door": _DOOR_TEXT, "window": _WINDOW_TEXT, "wall": _WALL_TEXT,
            "wall_organic": _WALL_ORGANIC_TEXT, "open_ground": "open paved ground",
            "ship": _SHIP_TEXT,
        },
        "enclosure": {f"{kind}_{part}": text
                      for kind, parts in _ENCLOSURE_WORDS.items()
                      for part, text in parts.items()},
        "phrasing": {
            "exact": _EXACT,
            "elaboration_exact": _ELABORATION["exact"],
            "elaboration_some": _ELABORATION["some"],
            "elaboration_free": _ELABORATION["free"],
            "effect_low": "faint and thin", "effect_medium": "clearly visible",
            "effect_high": "thick and dominating the area",
        },
        "terrain": dict(_TERRAIN_WORDS),
        "effects": dict(A.EFFECTS),
        "props": dict(_PROP_WORDS),
    }
    for section, values in merged.items():
        for key, value in (data.get(section) or {}).items():
            if isinstance(value, str) and value.strip():
                values[key] = value
    _PHRASES_CACHE = merged
    return merged


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
    rects.sort(key=lambda r: (-(r[2] * r[3]), r[1], r[0]))
    return rects


def _where_on_map(x, y, cols, rows):
    """A short phrase for where something stands, to tell two alike things apart."""
    fx, fy = (x + 0.5) / max(1, cols), (y + 0.5) / max(1, rows)
    band = "north" if fy < 0.34 else "south" if fy > 0.66 else ""
    side = "west" if fx < 0.34 else "east" if fx > 0.66 else ""
    if band and side:
        return f"In the {band}-{side} of the map"
    if band or side:
        return f"In the {band or side} of the map"
    return "Near the middle of the map"


def _the(label):
    """"The Crypt" already has its article; do not give it a second one."""
    return label if label[:4].lower() == "the " else "The " + label


def _upper_first(text):
    """Capitalise the first letter and leave every other one alone."""
    return text[:1].upper() + text[1:] if text else text


# Things there is no plural of: you have three heaps of rubble, not three
# rubbles, and a scatter of bones is already as plural as it gets.
_UNCOUNTED = {"rubble", "bones", "straw", "hay", "sand", "gravel", "moss", "water",
              "scree", "debris", "wreckage", "grass", "ash", "mud", "ice", "snow",
              "foliage", "undergrowth", "vegetation", "webbing", "rigging"}


def _plural(word, count):
    """Enough English to keep "3 torchs" and "5 boneses" out of the caption."""
    if count <= 1 or word.lower() in _UNCOUNTED:
        return word
    if word.endswith("s"):
        return word
    if word.endswith(("x", "z", "ch", "sh")):
        return word + "es"
    if word.endswith("y") and len(word) > 1 and word[-2] not in "aeiou":
        return word[:-1] + "ies"
    return word + "s"


def _wall_thickness(grid, bx, by, bw, bh):
    """How many squares deep the outer wall of this footprint actually is."""
    mid = by + bh // 2
    depth = 0
    for step in range(bw):
        if grid.get(bx + step, mid) in (A.WALL, A.DOOR, A.WINDOW):
            depth += 1
        else:
            break
    return max(1, depth)


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


def _components(grid, kinds):
    """8-connected blobs of the given tile kinds, as bounding rectangles."""
    seen = [[False] * grid.cols for _ in range(grid.rows)]
    out = []
    for sy in range(grid.rows):
        for sx in range(grid.cols):
            if seen[sy][sx] or grid.get(sx, sy) not in kinds:
                continue
            stack = [(sx, sy)]
            seen[sy][sx] = True
            x0 = x1 = sx
            y0 = y1 = sy
            n = 0
            while stack:
                x, y = stack.pop()
                n += 1
                x0, x1 = min(x0, x), max(x1, x)
                y0, y1 = min(y0, y), max(y1, y)
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        nx, ny = x + dx, y + dy
                        if (0 <= nx < grid.cols and 0 <= ny < grid.rows and not seen[ny][nx]
                                and grid.get(nx, ny) in kinds):
                            seen[ny][nx] = True
                            stack.append((nx, ny))
            out.append((x0, y0, x1 - x0 + 1, y1 - y0 + 1, n))
    out.sort(key=lambda r: r[2] * r[3], reverse=True)
    return out


def _largest_rects(mask, cols, rows, count, min_area):
    """Pull the biggest solid rectangles out of a boolean mask, largest first.

    Greedy row-merging leaves long thin slivers, which is why most of the open
    ground went undescribed. This finds real blocks instead.
    """
    grid = [row[:] for row in mask]
    out = []
    for _ in range(count):
        heights = [0] * cols
        best = (0, 0, 0, 0, 0)          # area, x, y, w, h
        for y in range(rows):
            for x in range(cols):
                heights[x] = heights[x] + 1 if grid[y][x] else 0
            stack = []
            for x in range(cols + 1):
                h = heights[x] if x < cols else 0
                start = x
                while stack and stack[-1][1] >= h:
                    sx, sh = stack.pop()
                    area = sh * (x - sx)
                    if area > best[0]:
                        best = (area, sx, y - sh + 1, x - sx, sh)
                    start = sx
                stack.append((start, h))
        if best[0] < min_area:
            break
        _, bx, by, bw, bh = best
        out.append((bx, by, bw, bh))
        for y in range(by, by + bh):
            for x in range(bx, bx + bw):
                grid[y][x] = False
    return out


def _elab(saying, value):
    """The 'how freely may you embellish this' phrase for one object."""
    key = str(value or "some").lower()
    return saying.get("elaboration_" + key, saying["elaboration_some"])


# Words that tell the renderer to divide an interior up. Harmless in a style
# meant for a house; ruinous in one applied to a single hall, because the style
# text lands in the caption background and outweighs any single element.
_DIVIDING_WORDS = ("partition", "divided into rooms", "divide the inside",
                   "separate rooms", "smaller rooms", "series of rooms",
                   "warren", "corridor")


def style_warnings(map_data, style):
    """Places where the chosen style argues with the plan it is painting."""
    out = []
    if not style:
        return out
    named = [a for a in (map_data.get("areas") or [])
             if str(a.get("label", "")).strip()]
    grid_cfg = map_data.get("grid") or {}
    cols = int(grid_cfg.get("cols", 0) or 0)
    rows = int(grid_cfg.get("rows", 0) or 0)
    text = " ".join(str(style.get(k, "")) for k in ("materials", "description")).lower()

    if len(named) == 1 and cols and rows:
        only = named[0]
        covers = (only.get("w", 0) * only.get("h", 0)) / max(1, cols * rows)
        if covers > 0.45:
            hits = []
            for w in _DIVIDING_WORDS:
                at = text.find(w)
                while at != -1:
                    lead = text[max(0, at - 60):at]
                    if not any(n in lead for n in (" no ", " not ", "never", "without",
                                                   "nor ", "n't ")):
                        hits.append(w)
                        break
                    at = text.find(w, at + 1)
            if hits:
                out.append(
                    f"style '{style.get('id', '?')}' describes interiors split up by "
                    f"{hits[0]}, but this map is one single room filling the field - the "
                    f"style text will fight the plan and the renderer will subdivide it. "
                    f"Pick a style written for one open space.")
    return out


def build_caption(map_data, style=None, base=None):
    """Return the structured caption as a dict."""
    grid = A.zones_to_grid(map_data)
    cols, rows = grid.cols, grid.rows
    meta = map_data.get("meta", {}) or {}
    style = style or {}
    base = base or {}

    phrases = load_phrases()
    wording = phrases["structure"]
    saying = phrases["phrasing"]
    exact = saying["exact"]

    layout_name = str(meta.get("layout", "")).lower()
    # What closes this site in decides what its edge is called, whether the map
    # may be described as a building at all, and what a way in looks like.
    enclosure = A.enclosure_of(style, layout_name)
    enc = dict(_ENCLOSURE_WORDS.get(enclosure) or _ENCLOSURE_WORDS["masonry"])
    for part in ("wall", "face", "boundary"):
        edited = (phrases.get("enclosure") or {}).get(f"{enclosure}_{part}")
        if edited:
            enc[part] = edited
        if str(style.get(part, "")).strip():
            enc[part] = str(style[part]).strip()

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
        clipped = " ".join(words[:44])
        stop = max(clipped.rfind(". "), clipped.rfind("; "), clipped.rfind(", "))
        head = clipped[:stop] if stop > len(clipped) // 2 else clipped
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
    # Diffusion text encoders handle negation badly, so the rule is stated as
    # what the walls are rather than as what they lack.
    opening_note = (
        f"Every wall in this picture is one continuous face of {enc['face']} from corner to "
        f"corner, interrupted only by {door_count} doorways and {window_count} windows, each "
        f"of which is listed below with its own rectangle. Everywhere else that face runs "
        f"straight on.")
    built = enclosure == "masonry"
    named_areas = [a for a in (map_data.get("areas") or [])
                   if str(a.get("label", "")).strip()]
    if len(named_areas) == 1:
        only = named_areas[0]
        covers = (only.get("w", 0) * only.get("h", 0)) / max(1, cols * rows)
        if covers > 0.45:
            caption["high_level_description"] += (
                f" The whole of this map is one single {'room' if built else 'open space'}, "
                f"{_the(only['label'])[0].lower()}{_the(only['label'])[1:]}, "
                f"and nothing else: "
                + ("one continuous floor from wall to wall with "
                   if built else "one continuous open ground with ")
                + f"no interior wall, no partition, no corridor and no smaller room "
                f"anywhere inside it. Everything standing on it is furniture and scenery, "
                f"not architecture.")

    caption["high_level_description"] += " " + opening_note
    # Nothing in the contract forbade perspective, so a scene that happened to
    # mention a ceiling came back drawn from the corner of the room.
    viewpoint = base.get("viewpoint_note")
    if viewpoint:
        caption["high_level_description"] += " " + viewpoint.rstrip(".") + "."

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
    atmosphere = []   # effects covering the whole map; they go in the background

    # The blank margin is asked for in the background text, and painted onto the
    # finished image afterwards by render.trim_to_margin - a box saying "nothing
    # is here" gives the renderer nothing to place, so it never worked.
    border = A.border_of(map_data)

    # 1. User-written annotations win over everything: they were placed by hand.
    for ann in map_data.get("annotations", []) or []:
        label = str(ann.get("label", "")).strip()
        if not label:
            continue
        text = label
        detail = str(ann.get("description", "")).strip()
        if detail:
            text += ". " + detail.rstrip(".")
        text += ". " + _elab(saying, ann.get("elaboration"))
        critical.append({"type": "obj",
                         "bbox": _bbox(int(ann.get("x", 0)), int(ann.get("y", 0)),
                                       max(1, int(ann.get("w", 1))),
                                       max(1, int(ann.get("h", 1))), cols, rows),
                         "desc": text + ". " + exact})

    # 2. Effects sit on top of everything, so they are described as overlays and
    #    must never be mistaken for a change of ground material.
    strength = {"low": saying["effect_low"], "medium": saying["effect_medium"],
                "high": saying["effect_high"]}
    for eff in map_data.get("effects", []) or []:
        kind = str(eff.get("kind", "")).lower()
        label = str(eff.get("label", "")).strip()
        body = phrases["effects"].get(kind, "")
        if label:
            text = label
            detail = str(eff.get("description", "")).strip()
            if detail:
                text += ". " + detail.rstrip(".")
            text += ". " + _elab(saying, eff.get("elaboration"))
        elif body:
            text = body[0].upper() + body[1:]
        else:
            continue
        how = strength.get(str(eff.get("intensity", "medium")).lower(), strength["medium"])
        ex, ey = int(eff.get("x", 0)), int(eff.get("y", 0))
        ew, eh = max(1, int(eff.get("w", 1))), max(1, int(eff.get("h", 1)))
        body_text = (f"{text}. This is an atmospheric effect painted over the scene, {how}, "
                     f"lying on top of the ground without replacing it")
        # An effect lying over most of the map is atmosphere, not a feature.
        # Handed over as boxes it came back as six identical elements, which is
        # both a quarter of the budget and the repeating pattern the rest of the
        # caption spends its length arguing against. It goes in the background
        # instead, which is where something covering everything belongs.
        if ew * eh >= 0.25 * cols * rows:
            atmosphere.append(f"{text}, {how}, lying over the whole map on top of "
                              f"everything else without replacing any of it")
            continue
        # One huge box makes the model paint the effect in a single corner and
        # call it done. Splitting a large area into tiles forces the coverage it
        # was asked for, because every tile has to be filled on its own.
        for (tx, ty, tw, th) in _tile_rect(ex, ey, ew, eh, cols, rows, max_tiles=4):
            spread = ("" if (tw, th) == (ew, eh) else
                      ", and this patch of it is one part of a single continuous effect "
                      "that covers the whole marked region")
            critical.append({
                "type": "obj",
                "bbox": _bbox(tx, ty, tw, th, cols, rows),
                "desc": f"{body_text}{spread}. It fills this whole rectangle. {exact}"})

    # 3. Structures.
    for st in map_data.get("structures", []) or []:
        if st.get("kind") != "ship":
            continue
        structure.append({
            "type": "obj",
            "bbox": _bbox(st["x"], st["y"], st["w"], st["h"], cols, rows),
            "desc": wording["ship"],
        })

    # 4. Doors. Few in number and load-bearing for how the map plays, so each one
    #    (see below - walls are emitted first, because openings only make sense
    #    once the runs they sit in exist)
    #    gets its own box - without this the renderer put openings where it liked.
    # Seen from above, a door shows only the top face of its leaf. Asking for a
    # "door leaf with a ring handle" asks for a front view, which is what made
    # the intended doors come out tilted and arched.
    door_word = style.get("door") or (
        "A closed door filling this opening in the wall, seen from directly overhead so "
        "only the flat top face of the door leaf is visible: a plain rectangular "
        "timber panel with dark iron bands across it, lying flush inside the wall "
        "opening and squared up with the wall, exactly as wide as the wall is thick. "
        "It is drawn flat, straight on and level with everything around it, with no "
        "perspective, no tilt, no arch or vault above it, no door frame standing "
        "proud, no steps and no visible handle")
    # 4b. Windows. Like doors, few and load-bearing: they say where the wall is
    #     broken by an opening, and the renderer will invent them anywhere if it
    #     is not told exactly where they belong.
    window_word = style.get("window") or wording["window"]
    for (x, y, w, h) in _merge_runs(grid, A.WINDOW):
        structure.append({"type": "obj", "bbox": _bbox(x, y, w, h, cols, rows),
                         "desc": f"{window_word}, filling the whole opening in the wall "
                                 f"and set flush into it, with solid wall continuing on "
                                 f"both sides. {exact}"})

    # 4b-2. Buildings first: the wall loop below skips anything already inside
    #       one of them.
    # Match a name to a footprint by where it sits, not by list order - the
    # areas are in creation order and include things that are not buildings at
    # all, which is how a warehouse came to be called "the Moored Ship".
    def _name_for(bx, by, bw, bh):
        """What to call this footprint, and what to say about its inside."""
        held = []
        for a in (map_data.get("areas") or []):
            label = str(a.get("label", "")).strip()
            if not label:
                continue
            ax = int(a.get("x", 0)) + int(a.get("w", 1)) / 2.0
            ay = int(a.get("y", 0)) + int(a.get("h", 1)) / 2.0
            if bx <= ax <= bx + bw and by <= ay <= by + bh:
                held.append((label, str(a.get("description", "")).strip()))
        if len(held) == 1:
            return f"the {held[0][0]}", held[0][1]
        if len(held) > 1:
            # Naming it after one of its rooms was a lie that cost the other
            # rooms their place in the caption. Say what it really is.
            names = ", ".join(name for name, _ in held[:-1]) + " and " + held[-1][0]
            return ("", 
                    f"Inside it, divided from each other by the interior walls listed "
                    f"below, are exactly {len(held)} rooms and no others: {names}. Each is "
                    f"described separately with its own rectangle")
        return "a building", ""

    organic = enclosure == "rock" or layout_name in ("forest", "swamp")
    hulls = [(int(st.get("x", 0)), int(st.get("y", 0)), int(st.get("w", 1)),
              int(st.get("h", 1)))
             for st in (map_data.get("structures") or []) if st.get("kind") == "ship"]

    def _in_hull(rx, ry, rw, rh):
        cx, cy = rx + rw / 2.0, ry + rh / 2.0
        return any(hx <= cx <= hx + hw and hy <= cy <= hy + hh
                   for (hx, hy, hw, hh) in hulls)

    building_rects = []
    building_names = []
    single_room_shell = {}   # building index -> the words its one room needs
    site_edge = None         # the walled ring that is the map itself, not a house
    if True:
        footprints = [r for r in _components(grid, (A.WALL, A.DOOR, A.WINDOW))
                      if r[4] >= 6 and not _in_hull(r[0], r[1], r[2], r[3])]
        # Put them in an order both ports agree on. Flood fill visits cells in
        # whatever order each language happens to, and two buildings swapping
        # places in the list is two captions that are not the same caption.
        footprints.sort(key=lambda r: (-r[4], r[1], r[0]))
        for i, (bx, by, bw, bh, _n) in enumerate(footprints[:5]):
            if bw < 3 or bh < 3:
                continue
            # A ring of wall round the whole map is the boundary of the site. On
            # anything but a building it is cliffs, treeline or fence, and
            # calling it "one single large building with its roof removed" is
            # how a gorge ended up as a masonry hall with a timber door in it.
            # Measured against the playable field, not the stored grid: every
            # map carries an empty bleed margin, so a ring drawn right round
            # the field starts two cells in from the corner of the picture.
            fb = A.border_of(map_data)
            field = max(1, (cols - 2 * fb) * (rows - 2 * fb))
            fills_map = (bw * bh >= field * 0.75 or
                         (bx <= fb + 1 and by <= fb + 1 and
                          bx + bw >= cols - fb - 1 and by + bh >= rows - fb - 1))
            if fills_map and enclosure != "masonry":
                site_edge = (bx, by, bw, bh)
                continue
            if organic:
                # Caves and woodland have no buildings in them; the loop runs
                # only to find the boundary of the site.
                continue
            building_rects.append((bx, by, bw, bh))
            size_word = ("large" if bw * bh > cols * rows * 0.12 else
                         "small" if bw * bh < cols * rows * 0.05 else "mid-sized")
            named, inside = _name_for(bx, by, bw, bh)
            if named == "a building":
                # Nothing named it, so name it by where it stands. Five
                # buildings all called "a building" read as one building drawn
                # five times.
                fx = (bx + bw / 2.0) / max(1, cols)
                fy = (by + bh / 2.0) / max(1, rows)
                band = ("north" if fy < 0.38 else "south" if fy > 0.62 else "middle")
                side = ("west" if fx < 0.38 else "east" if fx > 0.62 else "centre")
                where = (band if band != "middle" else "") +                         ("-" if band != "middle" and side != "centre" else "") +                         (side if side != "centre" else "")
                named = f"the {where} building" if where else "the middle building"
            building_names.append(named)
            # A count attached to the wall it belongs to holds far better than
            # one stated once for the whole map.
            doors_here = sum(1 for yy in range(by, by + bh) for xx in range(bx, bx + bw)
                             if grid.get(xx, yy) == A.DOOR)
            # Stated as what the wall is, not as what it lacks: diffusion text
            # encoders handle negation badly, and a count attached to the wall
            # it belongs to holds far better than one stated for the whole map.
            door_note = (
                f"One single plank-filled gap sits in its wall and the rest of that wall is "
                f"one continuous face of {enc['face']} running corner to corner"
                if doors_here == 1 else
                f"{doors_here} plank-filled gaps sit in its wall and the rest of that wall "
                f"is one continuous face of {enc['face']} running corner to corner"
                if doors_here else
                f"All four of its walls are one continuous face of {enc['face']} running "
                f"corner to corner")
            # How thick the wall actually is, said as a share of the picture.
            # "Thick outer walls" is an invitation, and the renderer accepted
            # it: a one-square wall came back as a band a sixth of the map
            # wide, and a band that wide is a place, so it got furnished.
            thick = _wall_thickness(grid, bx, by, bw, bh)
            share = max(2, int(round(100.0 * thick / max(bw, bh))))
            shell = (f"One single {size_word} building{', ' + named if named else ''}"
                     f", standing alone inside "
                     f"this rectangle and nowhere else: one unbroken outer wall running "
                     f"right round the edges of the rectangle, drawn as a narrow line about "
                     f"{share} percent of the width of this rectangle and no wider - a line, "
                     f"not a band - with its roof removed so the furnished floor inside is "
                     f"fully visible from above.")
            outside_note = (f"{door_note}. The inner face of those outer walls is flat and "
                            f"plain the whole way round, and the floor runs right up to it "
                            f"everywhere. The ground immediately outside the building on "
                            f"every side is open and free of any wall.")
            # Handing over the shell and the room as two rectangles, one inset
            # inside the other by the thickness of the wall, leaves a ring
            # between them that the renderer fills with alcoves it invented.
            # A building with one room in it is described once, at one rectangle.
            held_here = [a for a in (map_data.get("areas") or [])
                         if str(a.get("label", "")).strip()
                         and bx <= a["x"] + a["w"] / 2.0 <= bx + bw
                         and by <= a["y"] + a["h"] / 2.0 <= by + bh]
            if len(held_here) == 1:
                single_room_shell[len(building_rects) - 1] = (shell, outside_note)
                continue
            walls.append({
                "type": "obj",
                "bbox": _bbox(bx, by, bw, bh, cols, rows),
                "desc": (shell
                         + (f" {inside.rstrip('.')}." if inside else "")
                         + f" {outside_note} {exact}")})

    if site_edge is not None:
        # If the plan already says what the edge of this place is - cliffs down
        # both sides, a treeline along the top - then it has been said, and
        # saying it again in general terms only gives the renderer two answers.
        (sx, sy, sw, sh) = site_edge
        band = set()
        for yy in range(sy, sy + sh):
            for xx in range(sx, sx + sw):
                if xx in (sx, sx + sw - 1) or yy in (sy, sy + sh - 1) or                    grid.get(xx, yy) in (A.WALL, A.DOOR, A.WINDOW):
                    band.add((xx, yy))
        spoken = set()
        for note in (map_data.get("annotations") or []):
            nx, ny = int(note.get("x", 0)), int(note.get("y", 0))
            nw, nh = int(note.get("w", 1)), int(note.get("h", 1))
            for yy in range(ny, ny + nh):
                for xx in range(nx, nx + nw):
                    if (xx, yy) in band:
                        spoken.add((xx, yy))
        if band and len(spoken) >= 0.4 * len(band):
            site_edge = None
    if site_edge is not None:
        (sx, sy, sw, sh) = site_edge
        walls.append({
            "type": "obj",
            "bbox": _bbox(sx, sy, sw, sh, cols, rows),
            "desc": (f"The outer edge of the site, drawn as a continuous band right round "
                     f"the four sides of this rectangle and nowhere else: {enc['boundary']}. "
                     f"It is seen from directly overhead, so what is drawn is the flat top "
                     f"of it looking straight down and never its face; it is solid all the "
                     f"way through, and its edge against the ground inside is one clean "
                     f"continuous line the whole way round. It is the far limit of the map, "
                     f"not a building: nothing stands on it and nothing is set into it. "
                     f"Everything inside it is open ground. {exact}")})

    def _which_wall(x, y, w, h):
        """Which wall of which building - so no two doors read the same."""
        host = None
        host_name = ""
        for j, (bx, by, bw, bh) in enumerate(building_rects):
            if bx - 1 <= x and by - 1 <= y and x + w <= bx + bw + 1 and y + h <= by + bh + 1:
                host = (bx, by, bw, bh)
                host_name = building_names[j] if j < len(building_names) else ""
                break
        rx, ry, rw, rh = host if host else (0, 0, cols, rows)
        cxm, cym = x + w / 2.0, y + h / 2.0
        dx = (cxm - (rx + rw / 2.0)) / max(1.0, rw / 2.0)
        dy = (cym - (ry + rh / 2.0)) / max(1.0, rh / 2.0)
        side = ("east" if dx > 0 else "west") if abs(dx) > abs(dy) else                ("south" if dy > 0 else "north")
        where = f"in the {side} wall"
        if host_name and host_name != "a building":
            return f"{where} of {host_name}"
        return where + (" of this building" if host else " of the map")

    def _area_at(px, py):
        for a in (map_data.get("areas") or []):
            label = str(a.get("label", "")).strip()
            if not label:
                continue
            if (int(a["x"]) <= px < int(a["x"]) + int(a["w"]) and
                    int(a["y"]) <= py < int(a["y"]) + int(a["h"])):
                return label
        return ""

    def _door_place(x, y, w, h):
        """Which two rooms this doorway joins.

        Every door used to be called "in the east wall of this building", which
        for a door between two chambers is simply false - and the renderer
        answered it by punching holes in the outer wall where none belong.
        """
        # Look both ways along both axes: a one-cell doorway gives no clue which
        # wall it is in, and guessing from its shape put half of them in the
        # wrong wall.
        found = []
        for (dx, dy) in ((0, -1), (0, 1), (-1, 0), (1, 0)):
            for step in (1, 2):
                px = x + (w - 1 if dx > 0 else 0) + dx * step
                py = y + (h - 1 if dy > 0 else 0) + dy * step
                got = _area_at(px, py)
                if got:
                    if got not in found:
                        found.append(got)
                    break
        a = found[0] if found else ""
        b = found[1] if len(found) > 1 else ""
        def _lower_article(label):
            # Only the article, never the name: a room is called what it is
            # called, and "the crypt" is a different thing from "the Crypt".
            t = _the(label)
            return t[0].lower() + t[1:] if t else t

        if a and b and a != b:
            return (f"In the interior wall between {_lower_article(a)} and "
                    f"{_lower_article(b)}, and in no other wall")
        if a or b:
            return (f"In the outer wall of the building, the way in and out of "
                    f"{_lower_article(a or b)}")
        return _which_wall(x, y, w, h)

    for (x, y, w, h) in _merge_runs(grid, A.DOOR):
        structure.append({"type": "obj", "bbox": _bbox(x, y, w, h, cols, rows),
                         "desc": f"{_upper_first(_door_place(x, y, w, h))}: "
                                 f"{door_word[0].lower()}{door_word[1:]}. {exact}"})

    # 4c. Walls. Until now the renderer was handed room rectangles and left to
    #     invent every wall line itself, which is exactly where the layout came
    #     apart. The architect knows each run, so each run is given.
    # Caves and woodland have no straight walls to speak of; calling their rock
    # a "straight unbroken run" would be a lie the renderer would try to obey.
    # Merge each logical wall into one rectangle by treating its openings as
    # wall while the runs are found. Handing over the same wall as four separate
    # boxes reads as a repeating pattern, and the renderer answers a repeating
    # pattern with a symmetrical building it invented itself.
    solid = A.TileGrid(grid.cols, grid.rows, A.VOID)
    for yy in range(grid.rows):
        for xx in range(grid.cols):
            k = grid.get(xx, yy)
            solid.set(xx, yy, A.WALL if k in (A.WALL, A.DOOR, A.WINDOW) else k)
    wall_word = enc["wall"]
    wall_runs = _merge_runs(solid, A.WALL)
    for (x, y, w, h) in wall_runs[:(4 if organic else MAX_WALL_RUNS)]:
        # Only genuinely elongated runs. Everything else is a stub or, in a cave,
        # one lump of an irregular mass, and describing it as a wall adds noise.
        if max(w, h) < 3 or w * h < 2:
            continue
        on_outline = False
        outlines = ([(site_edge, 2)] if site_edge else []) +                    [(b, 1) for b in building_rects]
        # The boundary of a site is often two cells thick, so a run lying along
        # it can sit two cells in from the corner of its bounding box. Handed
        # over as a wall of its own, it becomes a second wall drawn just inside
        # the first one.
        for (bx, by, bw, bh), slack in outlines:
            inside = (bx - 1 <= x and by - 1 <= y and
                      x + w <= bx + bw + 1 and y + h <= by + bh + 1)
            if not inside:
                continue
            hugs_side = x <= bx + slack or x + w >= bx + bw - slack
            hugs_top = y <= by + slack or y + h >= by + bh - slack
            # Along an edge of the footprint, not across its middle.
            if (w >= h and hugs_top) or (h > w and hugs_side):
                on_outline = True
            break
        if on_outline:
            continue
        if organic:
            walls.append({
                "type": "obj",
                "bbox": _bbox(x, y, w, h, cols, rows),
                "desc": (f"A mass of {wall_word} filling this whole rectangle solidly, its "
                         f"face irregular and broken but with no passage, gap or opening "
                         f"through it anywhere. {exact}")})
            continue
        fx, fy = (x + w / 2.0) / max(1, cols), (y + h / 2.0) / max(1, rows)
        band = "north" if fy < 0.34 else "south" if fy > 0.66 else "middle"
        side = "west" if fx < 0.34 else "east" if fx > 0.66 else "centre"
        if w >= h:
            where = f"across the {band} of the map" if band != "middle" else                     "across the middle of the map"
            shape = (f"a horizontal wall running {where}, filling the full width of this "
                     f"rectangle from its left edge to its right edge")
        else:
            where = f"down the {side} of the map" if side != "centre" else                     "down the centre of the map"
            shape = (f"a vertical wall running {where}, filling the full height of this "
                     f"rectangle from its top edge to its bottom edge")
        walls.append({
            "type": "obj",
            "bbox": _bbox(x, y, w, h, cols, rows),
            "desc": (f"A run of {wall_word}: {shape}, filling it completely and keeping the "
                     f"same thickness along its entire length, with square ends and solid "
                     f"{enc['face']} everywhere except at the doors listed separately below. "
                     f"{exact}")})

    # 4d. The open ground. Without it the renderer treats every empty square as
    #     somewhere a building could go, and fills the map with invented rooms.
    open_word = style.get("ground") or wording["open_ground"]
    outside = [[grid.get(x, y) == A.FLOOR for x in range(cols)] for y in range(rows)]
    named_rects = [(int(a["x"]), int(a["y"]), int(a["w"]), int(a["h"]))
                   for a in (map_data.get("areas") or [])
                   if str(a.get("label", "")).strip()]
    for (bx, by, bw, bh) in building_rects + named_rects:
        for y in range(by, by + bh):
            for x in range(bx, bx + bw):
                if 0 <= y < rows and 0 <= x < cols:
                    outside[y][x] = False
    for (x, y, w, h) in _largest_rects(outside, cols, rows, 4,
                                       max(6, int(cols * rows * 0.012))):
        walls.append({
            "type": "obj",
            "bbox": _bbox(x, y, w, h, cols, rows),
            "desc": (f"Open ground of {open_word} filling this whole rectangle, unbroken "
                     f"from edge to edge: no building, no wall and no partition stands "
                     f"anywhere inside it, only loose objects lying on the ground. "
                     f"{exact}")})

    # 5. Terrain bodies large enough to matter, biggest first.
    def _already_described(x, y, w, h):
        """Has the scene already said what this is, in its own words?

        A lake the author called "absolutely still black water" does not want a
        second element calling the same rectangle "dark green water". Two
        answers to one question is worse than one, and it costs a slot.
        """
        covered = 0
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                for note in (map_data.get("annotations") or []):
                    nx, ny = int(note.get("x", 0)), int(note.get("y", 0))
                    nw, nh = int(note.get("w", 1)), int(note.get("h", 1))
                    if nx <= xx < nx + nw and ny <= yy < ny + nh:
                        covered += 1
                        break
        return covered >= 0.5 * max(1, w * h)

    for kind, phrase in phrases["terrain"].items():
        for (x, y, w, h) in _merge_runs(grid, kind)[:2]:
            if w * h < max(4, cols * rows * 0.02):
                continue
            if _already_described(x, y, w, h):
                continue
            normal.append({"type": "obj", "bbox": _bbox(x, y, w, h, cols, rows),
                           "desc": f"A body of {phrase} filling this region, its edge meeting "
                                   f"the surrounding ground in a clean line. {exact}"})

    # 6. Rooms.
    for area in map_data.get("areas", []) or []:
        label = (area.get("label") or "").strip()
        if not label or label.lower() in ("moored ship", "quay"):
            continue
        cx0, cy0 = area["x"] + area["w"] / 2.0, area["y"] + area["h"] / 2.0
        host_i = [j for j, (bx, by, bw, bh) in enumerate(building_rects)
                  if bx <= cx0 <= bx + bw and by <= cy0 <= by + bh]
        host = [building_rects[j] for j in host_i]
        if host:
            others = sum(1 for a2 in (map_data.get("areas", []) or [])
                         if a2 is not area and (a2.get("label") or "").strip()
                         and host[0][0] <= a2["x"] + a2["w"] / 2.0 <= host[0][0] + host[0][2]
                         and host[0][1] <= a2["y"] + a2["h"] / 2.0 <= host[0][1] + host[0][3])
            # Only a building with exactly one room in it is fully described by
            # its own element. With two or more, every room needs its own, or
            # the renderer is told the outline and left to invent the inside.
            single = not others
        else:
            single = False
        detail = str(area.get("description", "")).strip()
        # The one thing the renderer gets wrong on every single-room map: it
        # lines the inside of the outer wall with little alcoves. Said as what
        # the wall is rather than as what it lacks, and said here, against the
        # room itself, rather than once at the top of the caption.
        undivided = (
            "It is a single undivided space filling the whole of this rectangle from side "
            "to side: one unbroken floor, with the four walls around it and nothing else. "
            "The inner face of every one of those walls is flat and plain along its whole "
            "length, and the open floor runs right up to it on all four sides, so there is "
            "no interior wall, no partition, no screen, no alcove, no niche, no recess, no "
            "booth and no smaller room anywhere inside it")
        shell_here = None
        for j in host_i:
            if j in single_room_shell:
                shell_here = single_room_shell[j]
                break
        if shell_here is not None:
            # Described once, at the outer rectangle, so no ring is left over
            # between the shell and its interior for the renderer to fill in.
            bx, by, bw, bh = building_rects[host_i[0]]
            shell, outside_note = shell_here
            normal.append({
                "type": "obj",
                "bbox": _bbox(bx, by, bw, bh, cols, rows),
                "desc": (f"{shell} Inside those walls is {_the(label)[0].lower()}"
                         f"{_the(label)[1:]} and nothing else, "
                         f"seen from directly above with its floor and furniture fully "
                         f"visible and no roof, no ceiling and nothing overhanging it. "
                         f"{undivided}."
                         + (" " + _upper_first(detail.rstrip(".")) + "." if detail else "")
                         + f" {outside_note} {exact}"),
            })
            continue
        one_room = "" if not single else ". " + undivided
        normal.append({
            "type": "obj",
            "bbox": _bbox(area["x"], area["y"], area["w"], area["h"], cols, rows),
            "desc": (f"{_the(label)}: "
                     + ("the open ground of this place seen from directly above, filling "
                        "this rectangle, with nothing above it and nothing overhanging it"
                        if enclosure != "masonry" and not host else
                        "the roofless interior of a room seen from directly "
                        "above, its floor and furniture fully visible and filling this "
                        "rectangle, with no roof, no ceiling and nothing overhanging it")
                     + one_room
                     + (". " + detail.rstrip(".") if detail else "")),
        })

    # 7. Load-bearing props get their own box; clutter is described without one
    #    so the renderer can place it naturally.
    loose = []
    pinned = {}
    kind_counts = {}
    for f in map_data.get("features", []) or []:
        k = str(f.get("kind", "")).lower()
        kind_counts[k] = kind_counts.get(k, 0) + 1
    for f in map_data.get("features", []) or []:
        kind = str(f.get("kind", "")).lower()
        asked_for = not f.get("filler", False)
        if not f.get("structural", True) and not asked_for:
            loose.append(kind.replace("_", " "))
            continue
        label = str(f.get("label", "")).strip()
        if label:
            text = label
            detail = str(f.get("description", "")).strip()
            if detail:
                text += ". " + detail.rstrip(".")
            text += ". " + _elab(saying, f.get("elaboration"))
            critical.append({"type": "obj",
                             "bbox": _bbox(f["x"], f["y"], 1, 1, cols, rows),
                             "desc": text + ". " + exact})
            continue
        phrase = phrases["props"].get(kind)
        if not phrase:
            if not asked_for:
                continue
            # Asked for by name but nobody wrote a description for it. Its own
            # name is a poor description, and still better than silence.
            phrase = "a " + kind.replace("_", " ")
        if f.get("filler"):
            # Filler standing against a wall does not get a rectangle of its
            # own. A dozen small boxes ringing the inside of a room reads as a
            # row of compartments, and the renderer duly built one: three
            # renders of a single tavern hall came back lined with little
            # timber bays, one for each crate and barrel pinned to the wall.
            # It is still drawn - it goes into the clutter sentence, which says
            # what there is without saying where each piece stands.
            fx, fy = int(f["x"]), int(f["y"])
            if any(grid.get(fx + dx, fy + dy) in (A.WALL, A.DOOR, A.WINDOW)
                   for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                loose.append(kind.replace("_", " "))
                continue
            # Nor does filler get a box of its own on top of something the
            # scene has already described. Five heaps of rubble pinned inside
            # the rectangle of "a round stone well head standing alone" is two
            # instructions for one square, and the renderer has to pick.
            if any(int(n.get("x", 0)) <= fx < int(n.get("x", 0)) + int(n.get("w", 1))
                   and int(n.get("y", 0)) <= fy < int(n.get("y", 0)) + int(n.get("h", 1))
                   for n in (map_data.get("annotations") or [])):
                loose.append(kind.replace("_", " "))
                continue
            pinned[kind] = pinned.get(kind, 0) + 1
            if pinned[kind] > MAX_SAME_PROP:
                loose.append(kind.replace("_", " "))
                continue
        body = phrase if "from directly above" in phrase             else phrase + ", seen from directly above"
        if kind_counts.get(kind, 0) > 1:
            body = f"{_where_on_map(f['x'], f['y'], cols, rows)}, {body}"
        filler.append({"type": "obj",
                       "bbox": _bbox(f["x"], f["y"], 1, 1, cols, rows),
                       "desc": body})

    elements = critical + walls + structure + normal + filler
    budget = element_budget(map_data)
    if len(elements) > budget:
        elements = elements[:budget]

    if loose:
        counts = {}
        for k in loose:
            counts[k] = counts.get(k, 0) + 1
        listed = ", ".join(f"{n} {_plural(k, n)}"
                           for k, n in sorted(counts.items(),
                                              key=lambda kv: (-kv[1], kv[0]))[:6])
        elements.append({"type": "obj",
                         "desc": f"Scattered clutter across the walkable ground: {listed}, "
                                 f"standing about the floor, some of it against the walls "
                                 f"and some out in the open, casting soft shadows"})

    ground = style.get("ground") or "worn stone paving"
    # The style's material description used to be dropped entirely; it is the
    # richest source of theme detail we have, so it belongs in the background.
    materials = (style.get("materials") or "").strip()
    if materials:
        # A style describes a whole imagined scene - tents in a ring round a
        # fire, stalls along a street - and that description is the strongest
        # text in the caption, so it used to quietly overrule the plan it was
        # supposed to be painting. It is kept for its materials and colour and
        # told, in as many words, that it does not decide where anything goes.
        ground = (f"{ground}. The following names the kinds of thing this place is made of, "
                  f"for texture, material and colour only; it does not say where anything "
                  f"stands, and wherever it disagrees with the rectangles listed below, the "
                  f"rectangles are right and this is ignored: {materials.rstrip('.')}")
    suffix = base.get("background_suffix") or (
        "painted with visible brushwork, every part of the ground fully painted with no "
        "blank patches inside the map area")
    background = f"{ground} {suffix}"
    for note in atmosphere:
        background += f". {note}"

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

    # The ban opens the caption, but the caption is long and the last thing read
    # carries weight too. Two human figures once appeared in a map whose opening
    # sentence forbade them.
    background += ". " + forbidden

    # Said last, because until the elements are built nobody knows how many
    # buildings there are - and this sentence used to claim several of them on
    # a map with one room in it, which is an instruction, not a caveat. It is
    # how a single tavern hall came back ringed with little timber bays.
    count = len(building_rects)
    caption["high_level_description"] += (
        " The buildings are of different sizes and stand in an irregular arrangement."
        if count > 1 else
        " There is exactly one building in this picture and no second building anywhere."
        if count == 1 else "")
    # Said as what the halves are rather than as what the picture is not: the
    # renderer answered "not mirrored" with a map whose left half was a mirror
    # of its right, because the word it acted on was "mirrored".
    caption["high_level_description"] += (
        " The left half of this map and the right half are different from each other, and "
        "so are the top half and the bottom half: every part of the picture is its own "
        "shape, and each thing in it appears once, in one place, at its own angle.")

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
