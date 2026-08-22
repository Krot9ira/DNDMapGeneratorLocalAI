#!/usr/bin/env python3
"""
Deterministic map architect - the geometry engine of Stage 1.

The LLM (or an AI agent) only describes a scene *semantically*: what kind of
place it is, which areas exist, what terrain and props belong there. Every
spatial decision - room packing, corridor routing, wall derivation, door
placement, prop distribution - is made here, deterministically.

That split is deliberate: language models are unreliable at grid geometry (they
produce overlapping rectangles, walls that enclose nothing, and doors floating
in the middle of rooms), but they are good at describing places. This module
never emits a broken layout.

Public API:
    build(spec, seed=None)   -> map dict (schema-compatible with map.json)
    zones_to_grid(map_dict)  -> TileGrid  (rasterize rect zones back to tiles)
    SIZE_PRESETS             -> named grid sizes
"""
import json
import math
import random
from collections import deque

# -- Tile vocabulary --------------------------------------------------
VOID = "void"
FLOOR = "floor"
WALL = "wall"
DOOR = "door"
WINDOW = "window"
WATER = "water"
PIT = "pit"
RUBBLE = "rubble"
VEGETATION = "vegetation"
STAIRS = "stairs"
BRIDGE = "bridge"

# Zones are rasterized in this order, so later kinds paint over earlier ones.
PAINT_ORDER = [VOID, FLOOR, BRIDGE, STAIRS, WATER, PIT, RUBBLE,
               VEGETATION, WALL, DOOR, WINDOW]

TILE_KINDS = set(PAINT_ORDER)

# Tiles a creature can stand on - used for prop placement and connectivity.
WALKABLE = {FLOOR, BRIDGE, STAIRS, DOOR, RUBBLE, VEGETATION}

# The grid is free-form; these are only convenient starting points.
MIN_CELLS = 10
MAX_CELLS = 150

# An empty ring added outside the playable field, in cells. It is not part of
# the size the user picked - it is added on top of it. Two reasons: image models
# are least reliable at the very edge of a frame, so a margin means the mess
# happens in blank space instead of eating the corner of a room; and a printed
# battle map looks like this anyway, content inset from the paper edge.
BORDER_CELLS = 2

SIZE_PRESETS = {
    "small": (17, 13),
    "medium": (25, 19),
    "large": (66, 50),
    "huge": (100, 75),
    "giant": (150, 150),
}

LAYOUTS = ["dungeon", "building", "cavern", "open", "forest", "swamp", "ruins", "deck",
           "street", "district", "arena", "harbour", "custom"]

TERRAIN_KINDS = {"water", "pit", "rubble", "vegetation", "none"}

# Props that belong against a wall vs. out in the open. Placement quality is
# most of what makes a blueprint read as a real room rather than scattered dots.
WALL_PROPS = {
    "torch", "brazier", "bookshelf", "barrel", "crate", "bed", "sarcophagus",
    "anvil", "throne", "statue", "banner", "weapon_rack", "cabinet", "bar",
    "forge", "console", "locker", "dumpster", "chest", "coffin", "shelf",
}
CENTER_PROPS = {
    "altar", "fountain", "well", "campfire", "table", "portal", "crystal",
    "obelisk", "cauldron", "hearth", "stone_circle", "tree", "boulder",
    "mushroom", "stalagmite", "pillar",
}

# Atmospheric effects. They sit on a layer above everything else and never
# change what a square is made of or whether you can walk through it - they are
# light, smoke and weather painted over the finished ground.
EFFECTS = {
    "fire": "leaping orange flames with a bright hot core, throwing firelight across the "
            "surrounding ground",
    "embers": "a bed of glowing orange embers pulsing with heat, drifting sparks rising from it",
    "smoke": "thick grey smoke curling upward, the ground dimly visible through it",
    "fog": "a low bank of pale drifting fog, thinning at its edges, the ground still readable "
           "beneath it",
    "mist": "thin silver mist clinging low to the ground",
    "fireflies": "a scatter of tiny warm yellow-green points of light hanging in the air",
    "magic_glow": "a soft violet arcane glow washing over the ground, brightest at its centre",
    "holy_light": "a shaft of pale golden light falling from above onto the ground",
    "poison_gas": "a sickly yellow-green vapour lying heavy and low",
    "blood": "dark red blood pooled and smeared across the ground",
    "ice": "a sheet of pale blue ice with white frost feathering out from its edge",
    "webs": "sheets of dusty grey spider web strung across the space",
    "sparks": "bright white sparks arcing and scattering",
    "ash": "grey ash settled in drifts, more of it still falling",
    "steam": "white steam venting upward in soft billows",
    "shadow": "an unnatural pool of deep shadow that swallows the light",
}


def is_effect(kind):
    return str(kind).lower() in EFFECTS


# Every prop we have a concrete description or preview icon for.
KNOWN_PROPS = sorted(set(WALL_PROPS) | set(CENTER_PROPS) | {
    "crate", "barrel", "chest", "bones", "skull", "skeleton", "lamp", "lantern",
    "sconce", "cart", "wagon", "dumpster", "vending", "console", "locker", "net",
    "rope_coil", "bollard", "capstan", "mast", "gem", "shard", "bush", "shrub",
    "stump", "rock", "stone", "keg", "desk", "workbench", "chair", "stool",
    "coffin", "tomb", "shrine", "arch", "gate", "flag", "sign", "anvil", "forge",
    "bench", "crate_stack", "brazier", "torch",
})


# Props that genuinely shape play - they block movement, define a hall or are a
# tactical landmark - get their own bounding box in the caption, so the renderer
# must put them exactly there. Everything else (barrels, crates, bones, rope) is
# only described: pinning small clutter to an exact cell produced anonymous
# lumps, whereas the model paints convincing clutter on its own when it is
# merely told the clutter exists.
STRUCTURAL_PROPS = {
    "pillar", "column", "statue", "obelisk", "totem", "idol", "altar", "shrine",
    "sarcophagus", "coffin", "tomb", "table", "bed", "bunk", "throne", "anvil",
    "forge", "hearth", "well", "fountain", "pool", "tree", "boulder",
    "stalagmite", "crystal", "mast", "capstan", "portal", "gate", "arch",
    "bookshelf", "bar", "workbench", "cart", "wagon", "dumpster", "brazier",
    "campfire", "cauldron", "bench", "desk",
}


_CUSTOM_PROPS = None


def custom_props():
    """Object kinds somebody added to styles/_phrases.json themselves.

    Read straight from the file rather than through the caption builder, which
    imports this module. Cached: it is read once per run.
    """
    global _CUSTOM_PROPS
    if _CUSTOM_PROPS is None:
        _CUSTOM_PROPS = set()
        try:
            from paths import ROOT as _ROOT
            path = _ROOT / "styles" / "_phrases.json"
            if path.exists():
                data = json.loads(path.read_text(encoding="utf-8"))
                for key, value in (data.get("props") or {}).items():
                    if isinstance(value, str) and value.strip() and key not in KNOWN_PROPS:
                        _CUSTOM_PROPS.add(key)
        except (OSError, ValueError):
            _CUSTOM_PROPS = set()
    return _CUSTOM_PROPS


def is_structural_prop(kind):
    """Should this prop get its own bounding box, or be left to the renderer?"""
    name = str(kind).lower()
    # Something defined by hand was defined deliberately, so it is pinned and
    # described exactly rather than left to the renderer's judgement.
    return name in STRUCTURAL_PROPS or name in custom_props()


def normalize_prop(raw):
    """Map whatever the planner called a prop onto a kind we can actually draw.

    Language models write "wooden_barrels" and "mooring_bollards"; without this
    every such prop degrades to a featureless blob."""
    name = str(raw).strip().lower().replace(" ", "_").replace("-", "_")
    if not name:
        return ""
    if name in KNOWN_PROPS:
        return name
    # A kind somebody defined themselves is left exactly as written, or
    # "raspberry_bush" would collapse into "bush" and lose its description.
    if name in custom_props():
        return name
    if name.endswith("s") and name[:-1] in KNOWN_PROPS:
        return name[:-1]
    # Longest known kind inside the phrase wins, so "stacked_crates" resolves to
    # "crate" rather than to some shorter accidental match.
    best = ""
    for known in KNOWN_PROPS:
        if known in name and len(known) > len(best):
            best = known
    return best or name


class TileGrid:
    """Dense tile grid with a few convenience helpers."""

    def __init__(self, cols, rows, fill=VOID):
        self.cols = int(cols)
        self.rows = int(rows)
        self.cells = [[fill for _ in range(self.cols)] for _ in range(self.rows)]

    def inside(self, x, y):
        return 0 <= x < self.cols and 0 <= y < self.rows

    def get(self, x, y, default=VOID):
        if not self.inside(x, y):
            return default
        return self.cells[y][x]

    def set(self, x, y, kind):
        if self.inside(x, y):
            self.cells[y][x] = kind

    def fill_rect(self, x, y, w, h, kind):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.set(xx, yy, kind)

    def count(self, kind):
        return sum(row.count(kind) for row in self.cells)


# -- geometry helpers -------------------------------------------------

def _clamp(v, lo, hi):
    return max(lo, min(hi, v))


def _rect_center(r):
    x, y, w, h = r
    return (x + w // 2, y + h // 2)


def _bsp_split(rect, count, rng, min_leaf=7):
    """Split a rectangle into roughly `count` leaves, always splitting the
    largest splittable leaf so the result stays balanced."""
    leaves = [rect]
    guard = 0
    while len(leaves) < count and guard < 200:
        guard += 1
        leaves.sort(key=lambda r: r[2] * r[3], reverse=True)
        idx = None
        for i, (x, y, w, h) in enumerate(leaves):
            if w >= min_leaf * 2 or h >= min_leaf * 2:
                idx = i
                break
        if idx is None:
            break
        x, y, w, h = leaves.pop(idx)
        horizontal = w >= h
        if horizontal and w < min_leaf * 2:
            horizontal = False
        if not horizontal and h < min_leaf * 2:
            horizontal = True
        if horizontal:
            cut = rng.randint(min_leaf, w - min_leaf)
            leaves.append((x, y, cut, h))
            leaves.append((x + cut, y, w - cut, h))
        else:
            cut = rng.randint(min_leaf, h - min_leaf)
            leaves.append((x, y, w, cut))
            leaves.append((x, y + cut, w, h - cut))
    return leaves


def _largest_component(grid, kinds):
    """Return the set of cells in the largest 4-connected region of `kinds`."""
    seen = set()
    best = set()
    for y in range(grid.rows):
        for x in range(grid.cols):
            if (x, y) in seen or grid.get(x, y) not in kinds:
                continue
            comp = set()
            q = deque([(x, y)])
            seen.add((x, y))
            while q:
                cx, cy = q.popleft()
                comp.add((cx, cy))
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if (nx, ny) in seen or not grid.inside(nx, ny):
                        continue
                    if grid.get(nx, ny) in kinds:
                        seen.add((nx, ny))
                        q.append((nx, ny))
            if len(comp) > len(best):
                best = comp
    return best


def _carve_corridor(grid, a, b, rng):
    """Carve an L-shaped corridor between two points. Returns the path cells."""
    ax, ay = a
    bx, by = b
    if rng.random() < 0.5:
        legs = [((ax, ay), (bx, ay)), ((bx, ay), (bx, by))]
    else:
        legs = [((ax, ay), (ax, by)), ((ax, by), (bx, by))]
    path = []
    for (sx, sy), (ex, ey) in legs:
        step_x = 0 if ex == sx else (1 if ex > sx else -1)
        step_y = 0 if ey == sy else (1 if ey > sy else -1)
        cx, cy = sx, sy
        guard = 0
        while guard < 500:
            guard += 1
            if grid.inside(cx, cy):
                path.append((cx, cy))
            if (cx, cy) == (ex, ey):
                break
            cx += step_x
            cy += step_y
    for px, py in path:
        if grid.get(px, py) == VOID:
            grid.set(px, py, FLOOR)
    return path


def _derive_walls(grid):
    """Any void cell touching an open cell (8-neighbourhood) becomes wall.

    Deriving walls from the floor instead of trusting the LLM to place them is
    why every generated map is guaranteed to be properly enclosed."""
    open_kinds = (FLOOR, WATER, PIT, RUBBLE, VEGETATION, BRIDGE, STAIRS, DOOR)
    additions = []
    for y in range(grid.rows):
        for x in range(grid.cols):
            if grid.get(x, y) != VOID:
                continue
            touching = False
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    if grid.get(x + dx, y + dy) in open_kinds:
                        touching = True
                        break
                if touching:
                    break
            if touching:
                additions.append((x, y))
    for x, y in additions:
        grid.set(x, y, WALL)


def _open_edges(grid):
    """Strip the enclosing wall off the map border.

    Outdoor scenes - a dock, a street, a forest - should read as a slice of a
    larger world, not as a room with walls at the edge of the paper."""
    for x in range(grid.cols):
        for y in (0, grid.rows - 1):
            _dissolve_border(grid, x, y)
    for y in range(grid.rows):
        for x in (0, grid.cols - 1):
            _dissolve_border(grid, x, y)


def _dissolve_border(grid, x, y):
    if grid.get(x, y) != WALL:
        return
    for kind in (FLOOR, WATER, VEGETATION, RUBBLE):
        if any(grid.get(x + dx, y + dy) == kind
               for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
            grid.set(x, y, kind)
            return
    grid.set(x, y, FLOOR)


def _place_doors(grid, rooms, corridor_paths):
    """Turn the cell where a corridor meets a room into a door."""
    room_rects = [r["rect"] for r in rooms]

    def in_room(px, py):
        for i, (x, y, w, h) in enumerate(room_rects):
            if x <= px < x + w and y <= py < y + h:
                return i
        return -1

    doors = []
    for path in corridor_paths:
        if not path:
            continue
        prev_room = in_room(*path[0])
        for i in range(1, len(path)):
            cur = in_room(*path[i])
            if cur != prev_room:
                cell = path[i] if prev_room != -1 else path[i - 1]
                if grid.get(*cell) == FLOOR and cell not in doors:
                    sides = [grid.get(cell[0] + dx, cell[1] + dy)
                             for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))]
                    if sides.count(WALL) >= 2:
                        doors.append(cell)
            prev_room = cur
    for x, y in doors:
        grid.set(x, y, DOOR)
    return doors


def _fix_doors(grid):
    """A door only makes sense inside a wall run.

    Generators and terrain can leave a door tile with open ground on both sides,
    where it reads as a plain gap - and then the caption promises a door the
    renderer has nowhere to put. Each door is either seated properly in a wall or
    demoted to ordinary floor."""
    for y in range(grid.rows):
        for x in range(grid.cols):
            if grid.get(x, y) != DOOR:
                continue
            left, right = grid.get(x - 1, y), grid.get(x + 1, y)
            up, down = grid.get(x, y - 1), grid.get(x, y + 1)
            if left == WALL and right == WALL:
                continue
            if up == WALL and down == WALL:
                continue
            # Try to seat it: solid rock on both sides becomes the jamb.
            if left in (VOID, WALL) and right in (VOID, WALL):
                grid.set(x - 1, y, WALL)
                grid.set(x + 1, y, WALL)
            elif up in (VOID, WALL) and down in (VOID, WALL):
                grid.set(x, y - 1, WALL)
                grid.set(x, y + 1, WALL)
            else:
                grid.set(x, y, FLOOR)   # honest: this is an opening, not a door


def _blob(grid, cx, cy, radius, kind, rng, only_on=None, squish=1.0):
    """Organic blob used for pools, rubble fields and thickets."""
    r_out = int(radius + 2)
    for y in range(int(cy) - r_out, int(cy) + r_out + 1):
        for x in range(int(cx) - int(r_out * squish) - 1, int(cx) + int(r_out * squish) + 2):
            if not grid.inside(x, y):
                continue
            dx = (x - cx) / max(0.01, squish)
            dy = y - cy
            if math.hypot(dx, dy) <= radius * rng.uniform(0.8, 1.1):
                if only_on is None or grid.get(x, y) in only_on:
                    grid.set(x, y, kind)


# -- layout generators ------------------------------------------------

def _gen_dungeon(grid, spec, rng):
    """Discrete rooms connected by corridors, carved out of solid rock."""
    rooms_spec = spec["rooms"]
    n = _clamp(len(rooms_spec), 2, 9)
    interior = (1, 1, grid.cols - 2, grid.rows - 2)
    min_leaf = _clamp(min(grid.cols, grid.rows) // 3, 6, 12)
    leaves = _bsp_split(interior, n, rng, min_leaf=min_leaf)
    leaves.sort(key=lambda r: r[2] * r[3], reverse=True)

    weight = {"l": 3, "m": 2, "s": 1}
    order = sorted(range(len(rooms_spec)),
                   key=lambda i: -weight.get(str(rooms_spec[i].get("size", "m"))[:1].lower(), 2))

    rooms = []
    for slot in range(min(len(leaves), len(order))):
        lx, ly, lw, lh = leaves[slot]
        spec_room = rooms_spec[order[slot]]
        pad_x = rng.randint(1, 2) if lw > 9 else 1
        pad_y = rng.randint(1, 2) if lh > 9 else 1
        rx, ry = lx + pad_x, ly + pad_y
        rw, rh = lw - pad_x * 2, lh - pad_y * 2
        if rw < 3 or rh < 3:
            rx, ry = lx + 1, ly + 1
            rw, rh = lw - 2, lh - 2
        rw = min(rw, grid.cols - 1 - rx)
        rh = min(rh, grid.rows - 1 - ry)
        if rw < 3 or rh < 3:
            continue
        grid.fill_rect(rx, ry, rw, rh, FLOOR)
        rooms.append({"spec": spec_room, "rect": (rx, ry, rw, rh)})

    paths = _connect_rooms(grid, rooms, rng, loops=True)
    return rooms, paths


def _connect_rooms(grid, rooms, rng, loops=False):
    """Nearest-neighbour spanning tree over room centres keeps corridors short
    and guarantees the whole map is reachable."""
    paths = []
    if len(rooms) < 2:
        return paths
    connected = [0]
    remaining = list(range(1, len(rooms)))
    while remaining:
        best = None
        for ci in connected:
            for ri in remaining:
                ca = _rect_center(rooms[ci]["rect"])
                cb = _rect_center(rooms[ri]["rect"])
                d = abs(ca[0] - cb[0]) + abs(ca[1] - cb[1])
                if best is None or d < best[0]:
                    best = (d, ci, ri)
        _, ci, ri = best
        paths.append(_carve_corridor(grid, _rect_center(rooms[ci]["rect"]),
                                     _rect_center(rooms[ri]["rect"]), rng))
        connected.append(ri)
        remaining.remove(ri)
    if loops and len(rooms) >= 4:
        for _ in range(rng.randint(0, len(rooms) // 3)):
            a, b = rng.sample(range(len(rooms)), 2)
            paths.append(_carve_corridor(grid, _rect_center(rooms[a]["rect"]),
                                         _rect_center(rooms[b]["rect"]), rng))
    return paths


def _gen_building(grid, spec, rng):
    """One structure seen from above: outer shell, internal partition walls.

    Rooms are inset one cell inside their BSP leaf, so the untouched cells
    between leaves become the partition wall lattice automatically."""
    rooms_spec = spec["rooms"]
    n = _clamp(len(rooms_spec), 2, 8)
    interior = (1, 1, grid.cols - 2, grid.rows - 2)
    min_leaf = _clamp(min(grid.cols, grid.rows) // 4, 5, 10)
    leaves = _bsp_split(interior, n, rng, min_leaf=min_leaf)

    rooms = []
    for i, (lx, ly, lw, lh) in enumerate(leaves):
        if i >= len(rooms_spec):
            break
        rx, ry, rw, rh = lx + 1, ly + 1, lw - 1, lh - 1
        if rw < 2 or rh < 2:
            continue
        grid.fill_rect(rx, ry, rw, rh, FLOOR)
        rooms.append({"spec": rooms_spec[i], "rect": (rx, ry, rw, rh)})

    # Corridors between room centres punch single-cell gaps through the
    # partitions - exactly where interior doors belong.
    paths = _connect_rooms(grid, rooms, rng)

    # An outside door so the building is enterable.
    if rooms:
        entry = min(rooms, key=lambda r: r["rect"][1])
        rx, ry, rw, rh = entry["rect"]
        grid.set(rx + rw // 2, ry - 1, DOOR)
    return rooms, paths


def _gen_cavern(grid, spec, rng):
    """Chambers joined by winding passages, then softened into rock.

    Pure cellular automata kept collapsing below the safety threshold and
    falling back to a single oval, so every cave came out as one featureless
    blob. Chambers first, passages second, smoothing last: the result is always
    connected and always reads as a cave system.
    """
    grid.fill_rect(0, 0, grid.cols, grid.rows, VOID)
    span = min(grid.cols, grid.rows)

    n = _clamp(len(spec["rooms"]), 1, 6)
    centres = []
    rooms = []
    for i in range(n):
        # Spread the chambers around the map instead of clustering them.
        ang = (i / float(n)) * 2.0 * math.pi + rng.uniform(-0.4, 0.4)
        spread = rng.uniform(0.22, 0.36)
        cx = grid.cols / 2.0 + math.cos(ang) * grid.cols * spread
        cy = grid.rows / 2.0 + math.sin(ang) * grid.rows * spread
        size_hint = {"l": 0.30, "m": 0.24, "s": 0.18}.get(
            str(spec["rooms"][i].get("size", "m"))[:1], 0.24)
        radius = _clamp(span * size_hint * rng.uniform(0.9, 1.25), 3.0, span * 0.34)
        cx = _clamp(cx, radius + 2, grid.cols - radius - 2)
        cy = _clamp(cy, radius + 2, grid.rows - radius - 2)
        _blob(grid, cx, cy, radius, FLOOR, rng, squish=rng.uniform(0.7, 1.5))
        centres.append((int(cx), int(cy)))
        r = int(radius)
        rooms.append({"spec": spec["rooms"][i],
                      "rect": (_clamp(int(cx) - r // 2, 0, grid.cols - 2),
                               _clamp(int(cy) - r // 2, 0, grid.rows - 2),
                               max(3, r), max(3, r))})

    # Winding passages. A straight corridor would look quarried, so each leg is
    # carved and then blistered outwards at a few points.
    for i in range(1, len(centres)):
        path = _carve_corridor(grid, centres[i - 1], centres[i], rng)
        for j, cell in enumerate(path):
            # Widen the whole leg, and blister it out here and there.
            _blob(grid, cell[0], cell[1], rng.uniform(1.4, 2.0), FLOOR, rng)
            if j % max(2, len(path) // 5) == 0:
                _blob(grid, cell[0], cell[1], rng.uniform(2.2, 3.6), FLOOR, rng)
    if len(centres) > 2 and rng.random() < 0.7:
        # One loop, so a cave is not a dead-end tree.
        _carve_corridor(grid, centres[-1], centres[0], rng)

    # Two rounds of smoothing to eat the corners off, walls only.
    for _ in range(1):
        snapshot = [row[:] for row in grid.cells]
        for y in range(1, grid.rows - 1):
            for x in range(1, grid.cols - 1):
                solid = 0
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        if dx == 0 and dy == 0:
                            continue
                        if snapshot[y + dy][x + dx] == VOID:
                            solid += 1
                if snapshot[y][x] == FLOOR and solid >= 6:
                    grid.set(x, y, VOID)
                elif snapshot[y][x] == VOID and solid <= 2:
                    grid.set(x, y, FLOOR)

    # A rim of rock, so the cave never runs off the edge of the map.
    for x in range(grid.cols):
        for y in range(grid.rows):
            if x == 0 or y == 0 or x == grid.cols - 1 or y == grid.rows - 1:
                grid.set(x, y, VOID)

    # A cave has to be worth walking into. If the chambers landed small or on
    # top of each other, grow them until the field is properly hollowed out.
    field = grid.cols * grid.rows
    for _ in range(4):
        floor_now = sum(1 for y in range(grid.rows) for x in range(grid.cols)
                        if grid.get(x, y) != VOID)
        if floor_now >= field * 0.30:
            break
        for (ccx, ccy) in centres:
            _blob(grid, ccx, ccy, span * rng.uniform(0.16, 0.24), FLOOR, rng,
                  squish=rng.uniform(0.8, 1.4))

    # Roughen the rock face. The passages are carved along axes, so without
    # this a cave ends up with long straight edges and looks quarried.
    edge = []
    for y in range(1, grid.rows - 1):
        for x in range(1, grid.cols - 1):
            if grid.get(x, y) != VOID:
                continue
            if any(grid.get(x + dx, y + dy) == FLOOR
                   for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                edge.append((x, y))
    for cell in edge:
        if rng.random() < 0.32:
            grid.set(cell[0], cell[1], FLOOR)
    edge = []
    for y in range(1, grid.rows - 1):
        for x in range(1, grid.cols - 1):
            if grid.get(x, y) != VOID:
                continue
            if any(grid.get(x + dx, y + dy) == FLOOR
                   for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                edge.append((x, y))
    for cell in edge:
        if rng.random() < 0.18:
            grid.set(cell[0], cell[1], FLOOR)

    keep = _largest_component(grid, {FLOOR})
    for y in range(grid.rows):
        for x in range(grid.cols):
            if grid.get(x, y) == FLOOR and (x, y) not in keep:
                grid.set(x, y, VOID)

    # Loose stone where the passages meet the chambers.
    for _ in range(_clamp(grid.cols * grid.rows // 130, 2, 16)):
        _blob(grid, rng.uniform(0, grid.cols), rng.uniform(0, grid.rows),
              rng.uniform(1.0, 2.4), RUBBLE, rng, only_on={FLOOR})
    return rooms, []


def _gen_deck(grid, spec, rng):
    """One ship under way, open water on every side.

    `harbour` builds a quay and moors a vessel against it. A fight on the deck
    of a ship at sea is a different map: the deck is the whole playing field.
    """
    grid.fill_rect(0, 0, grid.cols, grid.rows, WATER)

    hull_w = _clamp(int(grid.cols * 0.82), 8, grid.cols - 2)
    hull_h = _clamp(int(grid.rows * 0.62), 5, grid.rows - 4)
    hx = (grid.cols - hull_w) // 2
    hy = (grid.rows - hull_h) // 2
    _carve_ship(grid, hx, hy, hull_w, hull_h, rng)

    # A mast step and a hatch coaming amidships, so the deck is not a bare oval.
    mid_y = hy + hull_h // 2
    for dx in (-1, 0, 1):
        grid.set(hx + hull_w // 2 + dx, mid_y, BRIDGE)

    # Fore, main and aft: the three places a boarding action happens.
    thirds = [("Forecastle", hx + int(hull_w * 0.70)),
              ("Main Deck", hx + int(hull_w * 0.38)),
              ("Quarterdeck", hx + int(hull_w * 0.10))]
    rooms = []
    for i, (fallback, cx) in enumerate(thirds):
        room = dict(spec["rooms"][i]) if i < len(spec["rooms"]) else {}
        room.setdefault("label", fallback)
        room.setdefault("id", fallback.lower().replace(" ", "_"))
        room.setdefault("size", "m")
        room.setdefault("props", [])
        w = max(3, hull_w // 5)
        h = max(3, hull_h // 2)
        rooms.append({"spec": room,
                      "rect": (_clamp(cx - w // 2, hx + 1, grid.cols - w - 1),
                               _clamp(mid_y - h // 2, hy + 1, grid.rows - h - 1), w, h)})

    spec.setdefault("structures", [])
    spec["structures"].append({"kind": "ship", "x": hx, "y": hy, "w": hull_w, "h": hull_h})
    return rooms, []


def _gen_open(grid, spec, rng):
    """Outdoor scene - no enclosing walls, terrain does the shaping."""
    grid.fill_rect(0, 0, grid.cols, grid.rows, FLOOR)
    for x in range(grid.cols):
        for y in range(grid.rows):
            edge = min(x, y, grid.cols - 1 - x, grid.rows - 1 - y)
            if edge == 0 or (edge == 1 and rng.random() < 0.55):
                grid.set(x, y, VEGETATION)

    rooms = []
    n = _clamp(len(spec["rooms"]), 1, 6)
    cols_n = 3 if n > 2 else n
    rows_n = max(1, math.ceil(n / cols_n))
    cw, ch = grid.cols // cols_n, grid.rows // rows_n
    for i in range(n):
        gx, gy = i % cols_n, i // cols_n
        rx = gx * cw + cw // 4
        ry = gy * ch + ch // 4
        rooms.append({"spec": spec["rooms"][i],
                      "rect": (rx, ry, max(3, cw // 2), max(3, ch // 2))})

    # A worn track through the scene gives the renderer something to follow.
    if grid.cols > 10:
        py = grid.rows // 2 + rng.randint(-1, 1)
        for x in range(grid.cols):
            wob = int(round(math.sin(x / 3.5) * 1.5))
            for dy in (-1, 0):
                grid.set(x, _clamp(py + wob + dy, 1, grid.rows - 2), RUBBLE)
    return rooms, []


def _gen_street(grid, spec, rng):
    """City block: a road with buildings pressed against it."""
    grid.fill_rect(0, 0, grid.cols, grid.rows, FLOOR)
    road_h = _clamp(grid.rows // 3, 4, 8)
    road_y = (grid.rows - road_h) // 2

    rooms = []
    specs = spec["rooms"]
    n = _clamp(len(specs), 2, 8)
    per_side = max(2, math.ceil(n / 2))
    available = grid.cols - 2
    bw = _clamp((available + 1) // per_side - 1, 5, 13)
    for side in (0, 1):
        top = 1 if side == 0 else road_y + road_h + 1
        h = (road_y - 1) if side == 0 else (grid.rows - top - 1)
        if h < 4:
            continue
        if bw < 5 or grid.cols < 12:
            continue
        for i in range(per_side):
            idx = side * per_side + i
            if idx >= n or idx >= len(specs):
                break
            bx = 1 + i * (bw + 1)
            if bx + bw > grid.cols - 1:
                break
            grid.fill_rect(bx, top, bw, h, VOID)
            grid.fill_rect(bx + 1, top + 1, bw - 2, h - 2, FLOOR)
            rooms.append({"spec": specs[idx], "rect": (bx + 1, top + 1, bw - 2, h - 2)})
            dx = bx + bw // 2
            dy = top + h - 1 if side == 0 else top
            grid.set(dx, dy, DOOR)
            # Some cottages open onto the side path instead of, or as well as,
            # the lane. A row of identical front doors reads as a barracks.
            if h >= 6 and rng.random() < 0.55:
                sx = bx if rng.random() < 0.5 else bx + bw - 1
                sy = top + rng.randrange(2, h - 2)
                grid.set(sx, sy, DOOR)

    # Too small for buildings either side: the road is the whole map, and one
    # named stretch of it is better than nothing to hang props on.
    if not rooms:
        rooms.append({"spec": specs[0] if specs else {"label": "Street", "size": "m",
                                                      "props": []},
                      "rect": (1, 1, max(2, grid.cols - 2), max(2, grid.rows - 2))})
    return rooms, []


def _gen_district(grid, spec, rng):
    """Buildings edge to edge, alleys between them, city to every margin."""
    grid.fill_rect(0, 0, grid.cols, grid.rows, FLOOR)

    # One lane of street round the outside, so the block never butts against
    # the frame and every building can be walked round.
    inner = (1, 1, grid.cols - 2, grid.rows - 2)
    if inner[2] < 6 or inner[3] < 6:
        rooms = [{"spec": (spec["rooms"] or [{"label": "Street", "size": "m", "props": []}])[0],
                  "rect": (1, 1, max(2, grid.cols - 2), max(2, grid.rows - 2))}]
        return rooms, []

    wanted = _clamp(len(spec["rooms"]) or 3, 2, 9)
    min_leaf = _clamp(min(grid.cols, grid.rows) // 4, 6, 14)
    leaves = _bsp_split(inner, wanted, rng, min_leaf=min_leaf)

    rooms = []
    for leaf in leaves:
        lx, ly, lw, lh = leaf
        # The alley is the gap left between one block and the next.
        bx, by = lx + 1, ly + 1
        bw, bh = lw - 2, lh - 2
        if bw < 4 or bh < 4:
            continue                       # too thin to be a building; leave it as street

        grid.fill_rect(bx, by, bw, bh, WALL)
        grid.fill_rect(bx + 1, by + 1, bw - 2, bh - 2, FLOOR)

        # Bigger houses get a partition, so an interior is not one bare box.
        if bw >= 9 and bh >= 6 and rng.random() < 0.7:
            px = bx + rng.randrange(3, bw - 3)
            for y in range(by + 1, by + bh - 1):
                grid.set(px, y, WALL)
            grid.set(px, by + 1 + rng.randrange(0, max(1, bh - 2)), DOOR)
        elif bh >= 9 and bw >= 6 and rng.random() < 0.7:
            py = by + rng.randrange(3, bh - 3)
            for x in range(bx + 1, bx + bw - 1):
                grid.set(x, py, WALL)
            grid.set(bx + 1 + rng.randrange(0, max(1, bw - 2)), py, DOOR)

        # A street door, on whichever side has an alley against it.
        sides = []
        if by > 1:
            sides.append(("n", bx + bw // 2, by))
        if by + bh < grid.rows - 1:
            sides.append(("s", bx + bw // 2, by + bh - 1))
        if bx > 1:
            sides.append(("w", bx, by + bh // 2))
        if bx + bw < grid.cols - 1:
            sides.append(("e", bx + bw - 1, by + bh // 2))
        rng.shuffle(sides)
        opened = False
        for _, dx, dy in sides:
            # Only a wall with open ground on the far side is a way out.
            out = ((dx, dy - 1) if dy == by else
                   (dx, dy + 1) if dy == by + bh - 1 else
                   (dx - 1, dy) if dx == bx else (dx + 1, dy))
            if grid.get(*out) == FLOOR:
                grid.set(dx, dy, DOOR)
                opened = True
                break
        if not opened:
            # Nothing faced an alley, so cut one through the nearest wall run.
            for yy in range(by, by + bh):
                for xx in range(bx, bx + bw):
                    if grid.get(xx, yy) != WALL:
                        continue
                    for ox, oy in ((xx + 1, yy), (xx - 1, yy), (xx, yy + 1), (xx, yy - 1)):
                        if grid.get(ox, oy) == FLOOR and not (bx < ox < bx + bw - 1 and
                                                              by < oy < by + bh - 1):
                            grid.set(xx, yy, DOOR)
                            opened = True
                            break
                    if opened:
                        break
                if opened:
                    break

        idx = len(rooms)
        spec_room = (spec["rooms"][idx] if idx < len(spec["rooms"])
                     else {"label": f"Building {idx + 1}", "size": "m", "props": []})
        rooms.append({"spec": spec_room,
                      "rect": (bx + 1, by + 1, max(2, bw - 2), max(2, bh - 2))})

    if not rooms:
        rooms.append({"spec": (spec["rooms"] or [{"label": "Street", "size": "m",
                                                  "props": []}])[0],
                      "rect": (1, 1, max(2, grid.cols - 2), max(2, grid.rows - 2))})
    return rooms, []


def _gen_arena(grid, spec, rng):
    """One dramatic chamber: a sand floor ringed by a colonnade and a gallery.

    It used to be a plain rectangle with two rows of pillars, which made it
    indistinguishable from `building` once the tiles were counted. An arena is a
    shape, not a room: a round fighting floor, a walled ring around it, and a
    way in from each side.
    """
    grid.fill_rect(0, 0, grid.cols, grid.rows, VOID)
    m = 1
    grid.fill_rect(m, m, grid.cols - m * 2, grid.rows - m * 2, FLOOR)

    cx, cy = grid.cols / 2.0, grid.rows / 2.0
    radius = min(grid.cols, grid.rows) / 2.0 - 3.0

    if radius >= 3.0:
        # The barrier: a ring of wall with four gates onto the sand.
        for y in range(grid.rows):
            for x in range(grid.cols):
                dx = (x - cx) / max(1.0, radius)
                dy = (y - cy) / max(1.0, radius * 0.82)
                d = math.sqrt(dx * dx + dy * dy)
                if 0.97 <= d <= 1.06:
                    grid.set(x, y, WALL)
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            for lane in (-1, 0, 1):
                x = int(cx) + (lane if dy else 0)
                y = int(cy) + (lane if dx else 0)
                for _ in range(max(grid.cols, grid.rows)):
                    x += dx
                    y += dy
                    if not grid.inside(x, y):
                        break
                    if grid.get(x, y) == WALL:
                        grid.set(x, y, FLOOR)
        # Sand inside the barrier, so the fighting floor reads differently from
        # the gallery walkway outside it.
        for y in range(grid.rows):
            for x in range(grid.cols):
                dx = (x - cx) / max(1.0, radius)
                dy = (y - cy) / max(1.0, radius * 0.82)
                if math.sqrt(dx * dx + dy * dy) < 0.95 and grid.get(x, y) == FLOOR:
                    grid.set(x, y, RUBBLE)

    specs = spec["rooms"] or [{"id": "arena", "label": "Arena", "size": "l", "props": []}]
    r = max(2, int(radius * 0.7))
    rooms = [{"spec": specs[0], "rect": (_clamp(int(cx) - r, 1, grid.cols - 3),
                                         _clamp(int(cy) - r // 2, 1, grid.rows - 3),
                                         max(3, r * 2), max(3, r))}]
    # A holding cell or two off the gallery, if there is room and the caller
    # asked for more than one area.
    for i, extra in enumerate(specs[1:3]):
        side = -1 if i % 2 == 0 else 1
        w = max(3, grid.cols // 7)
        h = max(3, grid.rows // 6)
        rx = _clamp(int(cx + side * (grid.cols / 2.0 - w / 2.0 - 1)) - w // 2, 1,
                    grid.cols - w - 1)
        ry = _clamp(int(cy) - h // 2, 1, grid.rows - h - 1)
        rooms.append({"spec": extra, "rect": (rx, ry, w, h)})
    return rooms, []


def _gen_harbour(grid, spec, rng):
    """Waterfront: paved quay on land, open water along one edge, and one large
    ship floating on that water with a gangway onto the dock.

    The hull is the whole point of this layout - a hard, recognisable silhouette
    gives the diffusion model something unambiguous to paint. The map edges are
    deliberately left open: no enclosing wall, the scene just continues."""
    water_frac = _clamp(float(spec.get("water_fraction", 0.32)), 0.2, 0.6)
    split = _clamp(int(round(grid.rows * (1.0 - water_frac))), 5, grid.rows - 5)

    # Paved land on top, open harbour water along the bottom edge.
    grid.fill_rect(0, 0, grid.cols, grid.rows, FLOOR)
    grid.fill_rect(0, split, grid.cols, grid.rows - split, WATER)
    quay_y = split - 1

    # One large ship, broadside to the quay, floating clear of the dock edge.
    rooms = []
    water_rows = grid.rows - split
    hull_h = _clamp(water_rows - 2, 4, 9)
    hull_w = _clamp(int(grid.cols * 0.55), 8, grid.cols - 4)
    hx = (grid.cols - hull_w) // 2
    hy = split + 1
    if hy + hull_h > grid.rows:
        hull_h = grid.rows - hy
    _carve_ship(grid, hx, hy, hull_w, hull_h, rng)
    spec.setdefault("structures", [])
    spec["structures"].append({"kind": "ship", "x": hx, "y": hy,
                               "w": hull_w, "h": hull_h, "facing": "e"})
    rooms.append({"spec": {"id": "ship", "label": "Moored Ship",
                           "props": ["mast", "capstan", "crate", "barrel", "rope_coil"]},
                  "rect": (hx + 2, hy + 1, max(2, hull_w - 4), max(2, hull_h - 2))})

    # Gangway from the deck up onto the quay - the bridge the scene calls for.
    gang_x = hx + hull_w // 2
    for y in range(quay_y + 1, hy + 1):
        grid.set(gang_x, y, BRIDGE)
        grid.set(gang_x + 1, y, BRIDGE)

    # Warehouses set back from the quay so the dock itself stays walkable.
    # Drop any area the planner named after the vessel - this generator already
    # built the ship, and a warehouse labelled "Moored Ship" reads as a bug.
    ship_words = ("ship", "vessel", "boat", "galleon", "hull", "deck")
    land_specs = [r for r in spec["rooms"]
                  if not any(w in (str(r.get("label", "")) + str(r.get("id", ""))).lower()
                             for w in ship_words)][:4]
    build_h = quay_y - 2
    if build_h >= 5 and land_specs:
        leaves = _bsp_split((1, 1, grid.cols - 2, build_h), len(land_specs), rng,
                            min_leaf=_clamp(grid.cols // 5, 5, 10))
        for i, (lx, ly, lw, lh) in enumerate(leaves):
            if i >= len(land_specs) or lw < 5 or lh < 5:
                continue
            bw, bh = lw - 2, lh - 2
            grid.fill_rect(lx + 1, ly + 1, bw, bh, VOID)
            grid.fill_rect(lx + 2, ly + 2, bw - 2, bh - 2, FLOOR)
            rooms.append({"spec": land_specs[i],
                          "rect": (lx + 2, ly + 2, max(2, bw - 2), max(2, bh - 2))})
            # Door sits in the seaward wall itself, not on the cobbles beyond it.
            grid.set(lx + 1 + bw // 2, ly + bh, DOOR)

    # Mooring bollards lining the dock edge.
    spec.setdefault("features", [])
    for x in range(2, grid.cols - 2, 5):
        if grid.get(x, quay_y) == FLOOR:
            spec["features"].append({"kind": "bollard", "x": x, "y": quay_y})

    # The open dock itself is an area too. Without this the quay is a large
    # empty field and the renderer has nothing to anchor it to, which is how
    # it ends up painted as blank background.
    dock_top = max(1, quay_y - 3)
    rooms.append({"spec": {"id": "quay", "label": "Quay",
                           "props": ["crate", "barrel", "cart", "rope_coil",
                                     "net", "crate", "barrel"]},
                  "rect": (1, dock_top, grid.cols - 2, quay_y - dock_top)})
    return rooms, []


def _carve_ship(grid, hx, hy, w, h, rng=None):
    """A hull with a tapered bow and stern: planking as wall, deck as floor.

    Laid out broadside (long axis horizontal) so a wide map reads as one big
    vessel rather than a thin sliver."""
    half = (h - 1) / 2.0
    bow = _clamp(int(w * 0.34), 3, 10)
    stern = _clamp(int(w * 0.16), 2, 5)

    insets = []
    for i in range(w):
        if i >= w - bow:
            # Elliptical bow: barely narrows at first, then sweeps to a point.
            t = _clamp((i - (w - bow) + 1) / float(bow), 0.0, 1.0)
            inset = half * (1.0 - math.sqrt(max(0.0, 1.0 - t * t)))
        elif i < stern:
            # Flat transom stern, only slightly drawn in.
            t = (stern - i) / float(stern + 1)
            inset = t * half * 0.45
        else:
            inset = 0.0
        insets.append(max(0, int(round(inset))))

    for i, inset in enumerate(insets):
        y0, y1 = hy + inset, hy + h - inset
        if y1 - y0 < 1:
            continue
        for y in range(y0, y1):
            grid.set(hx + i, y, WALL)
    # Hollow out the deck one plank inside the hull. BRIDGE, not FLOOR: it
    # carries plank texture and its own depth level, so the vessel reads as
    # timber instead of as another stretch of stone quay.
    for i in range(1, w - 1):
        inset = insets[i] + 1
        y0, y1 = hy + inset, hy + h - inset
        for y in range(y0, y1):
            grid.set(hx + i, y, BRIDGE)


def _gen_forest(grid, spec, rng):
    """Dense woodland: the default state is thicket, and clearings are carved
    out of it. The opposite of `open`, where the default is bare ground."""
    grid.fill_rect(0, 0, grid.cols, grid.rows, VEGETATION)

    rooms = []
    n = _clamp(len(spec["rooms"]), 1, 6)
    span = min(grid.cols, grid.rows)
    for i in range(n):
        spec_room = spec["rooms"][i]
        size_hint = {"l": 0.20, "m": 0.14, "s": 0.09}.get(str(spec_room.get("size", "m"))[:1], 0.14)
        radius = _clamp(span * size_hint, 2.0, 14.0)
        cx = rng.uniform(radius + 1, grid.cols - radius - 1)
        cy = rng.uniform(radius + 1, grid.rows - radius - 1)
        _blob(grid, cx, cy, radius, FLOOR, rng, squish=rng.uniform(0.8, 1.4))
        r = int(radius)
        rooms.append({"spec": spec_room,
                      "rect": (_clamp(int(cx) - r // 2, 0, grid.cols - 2),
                               _clamp(int(cy) - r // 2, 0, grid.rows - 2),
                               max(3, r), max(3, r))})

    # Trodden paths joining the clearings, so the scene is actually crossable.
    for i in range(1, len(rooms)):
        for cell in _carve_corridor(grid, _rect_center(rooms[i - 1]["rect"]),
                                    _rect_center(rooms[i]["rect"]), rng):
            grid.set(cell[0], cell[1], RUBBLE)
            if rng.random() < 0.5:
                grid.set(cell[0], cell[1] + rng.choice((-1, 1)), RUBBLE)

    # A scatter of thicker undergrowth so the canopy is not uniform.
    for _ in range(_clamp(grid.cols * grid.rows // 120, 2, 20)):
        _blob(grid, rng.uniform(0, grid.cols), rng.uniform(0, grid.rows),
              rng.uniform(1.5, 4.0), VEGETATION, rng, only_on={FLOOR})
    return rooms, []


def _gen_swamp(grid, spec, rng):
    """Standing water broken by reed beds and tussocks of solid ground."""
    grid.fill_rect(0, 0, grid.cols, grid.rows, WATER)
    span = min(grid.cols, grid.rows)

    rooms = []
    n = _clamp(len(spec["rooms"]), 1, 6)
    for i in range(n):
        radius = _clamp(span * rng.uniform(0.10, 0.18), 2.0, 12.0)
        cx = rng.uniform(radius + 1, grid.cols - radius - 1)
        cy = rng.uniform(radius + 1, grid.rows - radius - 1)
        _blob(grid, cx, cy, radius, FLOOR, rng, squish=rng.uniform(0.7, 1.5))
        r = int(radius)
        rooms.append({"spec": spec["rooms"][i],
                      "rect": (_clamp(int(cx) - r // 2, 0, grid.cols - 2),
                               _clamp(int(cy) - r // 2, 0, grid.rows - 2),
                               max(3, r), max(3, r))})

    # Reed beds along the waterline.
    for _ in range(_clamp(grid.cols * grid.rows // 90, 3, 26)):
        _blob(grid, rng.uniform(0, grid.cols), rng.uniform(0, grid.rows),
              rng.uniform(1.5, 4.0), VEGETATION, rng, only_on={WATER})

    # Plank walkways between the islands - a swamp you cannot cross is useless.
    for i in range(1, len(rooms)):
        for cell in _carve_corridor(grid, _rect_center(rooms[i - 1]["rect"]),
                                    _rect_center(rooms[i]["rect"]), rng):
            if grid.get(*cell) in (WATER, VEGETATION):
                grid.set(cell[0], cell[1], BRIDGE)
    return rooms, []


def _gen_ruins(grid, spec, rng):
    """An open site strewn with fragments of collapsed building."""
    grid.fill_rect(0, 0, grid.cols, grid.rows, FLOOR)
    for x in range(grid.cols):
        for y in range(grid.rows):
            edge = min(x, y, grid.cols - 1 - x, grid.rows - 1 - y)
            if edge == 0 or (edge == 1 and rng.random() < 0.4):
                grid.set(x, y, VEGETATION)

    rooms = []
    n = _clamp(len(spec["rooms"]), 2, 7)
    leaves = _bsp_split((2, 2, grid.cols - 4, grid.rows - 4), n, rng,
                        min_leaf=_clamp(min(grid.cols, grid.rows) // 5, 4, 12))
    for i, (lx, ly, lw, lh) in enumerate(leaves):
        if i >= len(spec["rooms"]) or lw < 4 or lh < 4:
            continue
        rx, ry = lx + 1, ly + 1
        rw, rh = lw - 2, lh - 2
        # Broken outline: each wall run survives only in pieces.
        for x in range(rx, rx + rw):
            if rng.random() < 0.65:
                grid.set(x, ry, WALL)
            if rng.random() < 0.65:
                grid.set(x, ry + rh - 1, WALL)
        for y in range(ry, ry + rh):
            if rng.random() < 0.65:
                grid.set(rx, y, WALL)
            if rng.random() < 0.65:
                grid.set(rx + rw - 1, y, WALL)
        rooms.append({"spec": spec["rooms"][i], "rect": (rx + 1, ry + 1,
                                                         max(2, rw - 2), max(2, rh - 2))})

    for _ in range(_clamp(grid.cols * grid.rows // 100, 3, 24)):
        _blob(grid, rng.uniform(0, grid.cols), rng.uniform(0, grid.rows),
              rng.uniform(1.0, 3.0), RUBBLE, rng, only_on={FLOOR})
    return rooms, []


# Words that name a built interior. On an open-air site everything else is
# outdoors, and outdoors does not have walls round it.
_BUILT_WORDS = (
    "house", "hut", "cottage", "cabin", "barn", "shed", "stable", "granary",
    "mill", "forge", "smithy", "workshop", "warehouse", "store", "storeroom",
    "shop", "inn", "tavern", "hall", "keep", "tower", "turret", "gatehouse",
    "chapel", "church", "temple", "shrine", "crypt", "vault", "cellar",
    "room", "chamber", "kitchen", "bedroom", "study", "library", "office",
    "guardhouse", "barracks", "lodge", "cabin", "manor", "villa", "interior",
)
# ...and words that name an outdoor space, for when both could be read into it.
_OPEN_WORDS = (
    "street", "square", "yard", "courtyard", "market", "plaza", "green",
    "field", "meadow", "clearing", "glade", "shore", "bank", "beach", "path",
    "road", "track", "lane", "terrace", "garden", "grove", "camp", "dock",
    "quay", "pier", "bridge", "ford", "crossing", "floor", "ground", "mire",
    "bog", "marsh", "gorge", "pass", "ravine", "plain", "slope", "rise",
)


def _room_is_built(room_spec):
    """Does this room have walls round it, or is it a piece of the open air?

    A city street map is a street with houses on it, not a building with three
    rooms in it - but every room used to be walled and doored alike, so a
    fountain square came out as a chamber with a fountain in it. A scene can say
    outright with `enclosed`; otherwise the name decides, and outdoors wins,
    because on an open-air site that is what the ground is unless something was
    built on it.
    """
    if isinstance(room_spec, dict) and room_spec.get("enclosed") is not None:
        return bool(room_spec.get("enclosed"))
    text = " ".join(str((room_spec or {}).get(k, "")) for k in ("label", "description"))
    text = text.lower()
    first = str((room_spec or {}).get("label", "")).lower()
    for word in _OPEN_WORDS:
        if word in first:
            return False
    for word in _BUILT_WORDS:
        if word in first:
            return True
    for word in _OPEN_WORDS:
        if word in text:
            return False
    return any(word in text for word in _BUILT_WORDS)


def _open_up_outdoor_rooms(grid, rooms, enclosure):
    """Join the outdoor rooms of an open-air site into one continuous surface.

    Walls are derived from where floor meets nothing, so two rooms with a gap
    between them get a wall each and a door between them. Outdoors there is no
    gap: the square runs into the street. The ground between them is filled in
    before the walls are worked out, and anything actually built keeps its own.
    """
    if enclosure != "open":
        return
    outdoor = [r for r in rooms if not _room_is_built(r.get("spec"))]
    built = [r for r in rooms if _room_is_built(r.get("spec"))]
    if not outdoor:
        return
    keep = set()
    for r in built:
        bx, by, bw, bh = r["rect"]
        for yy in range(by - 1, by + bh + 1):
            for xx in range(bx - 1, bx + bw + 1):
                keep.add((xx, yy))
    # Right out to the edge of the field, not merely between the rooms. A wall
    # is derived wherever floor meets nothing, so a street laid two squares in
    # from the edge came out ringed by one - and dissolving the outermost row
    # afterwards only took the outer half of it. Outdoors the ground does not
    # stop at a wall; it runs off the map.
    #
    # Spread from the outdoor ground rather than filling everything, and stop
    # at anything already placed. A clearing ringed by its own treeline should
    # not grow a strip of walkable ground on the far side of the trees.
    stack = []
    for r in outdoor:
        rx, ry, rw, rh = r["rect"]
        for yy in range(ry, ry + rh):
            for xx in range(rx, rx + rw):
                # Only from ground you can actually stand on. Seeding from every
                # square of the rectangle meant seeding from the treeline the
                # scene had just laid across it, and the ground spread out
                # through the trees to the far side of them.
                if grid.get(xx, yy) in WALKABLE:
                    stack.append((xx, yy))
    seen = set(stack)
    while stack:
        x, y = stack.pop()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if not grid.inside(nx, ny) or (nx, ny) in seen or (nx, ny) in keep:
                continue
            if grid.get(nx, ny) != VOID:
                continue
            seen.add((nx, ny))
            grid.set(nx, ny, FLOOR)
            stack.append((nx, ny))


def _gen_custom(grid, spec, rng):
    """Explicit rectangles supplied by the caller - validated, then walled."""
    rooms = []
    for r in spec["rooms"]:
        try:
            rx = _clamp(int(r["x"]), 0, grid.cols - 2)
            ry = _clamp(int(r["y"]), 0, grid.rows - 2)
            rw = _clamp(int(r["w"]), 2, grid.cols - rx)
            rh = _clamp(int(r["h"]), 2, grid.rows - ry)
        except (KeyError, TypeError, ValueError):
            continue
        grid.fill_rect(rx, ry, rw, rh, FLOOR)
        rooms.append({"spec": r, "rect": (rx, ry, rw, rh)})
    if not rooms:
        return _gen_dungeon(grid, spec, rng)
    paths = _connect_rooms(grid, rooms, rng)
    return rooms, paths


_GENERATORS = {
    "dungeon": _gen_dungeon,
    "building": _gen_building,
    "cavern": _gen_cavern,
    "open": _gen_open,
    "street": _gen_street,
    "arena": _gen_arena,
    "harbour": _gen_harbour,
    "forest": _gen_forest,
    "swamp": _gen_swamp,
    "ruins": _gen_ruins,
    "deck": _gen_deck,
    "district": _gen_district,
    "custom": _gen_custom,
}


# -- terrain, connectivity and props ----------------------------------

_AMOUNT_SCALE = {"none": 0.0, "low": 0.5, "light": 0.5, "medium": 1.0,
                 "med": 1.0, "high": 1.6, "heavy": 1.6}

# Used when neither the caller nor the style names any props, so a map is never
# an empty box.
_DEFAULT_FILLER = {
    "dungeon": ["pillar", "torch", "crate", "barrel", "chest", "brazier", "rubble"],
    "building": ["table", "chair", "barrel", "crate", "bookshelf", "bed", "hearth"],
    "cavern": ["stalagmite", "boulder", "crystal", "mushroom", "bones"],
    "open": ["tree", "boulder", "campfire", "bush", "stump"],
    "street": ["crate", "barrel", "dumpster", "lamp", "cart"],
    "arena": ["pillar", "brazier", "statue", "bones", "weapon_rack"],
    "harbour": ["crate", "barrel", "rope_coil", "bollard", "cart", "net", "lamp"],
    "forest": ["tree", "tree", "bush", "boulder", "stump", "mushroom", "campfire"],
    "swamp": ["tree", "bush", "stump", "boulder", "bones", "mushroom"],
    "ruins": ["boulder", "rubble", "statue", "pillar", "bush", "bones", "crate"],
    "custom": ["pillar", "torch", "crate", "barrel"],
}


# What a named region is made of, when its name says so plainly. An annotation
# used to be caption text and nothing else, so a plan could have open floor
# where the words said there was a colonnade, a stair, a lake or a cliff - and
# the renderer, handed both, had to choose. Keyed on the label, because a
# description mentions the cliff a tent is pitched against without being one.
_ANNOTATION_GROUND = (
    (("cliff", "rock face", "curtain wall", "sea wall", "rampart", "palisade",
      "stockade", "bulwark", "mirror wall", "forest wall"), WALL),
    (("stair", "steps", "stairway"), STAIRS),
    (("bridge", "boardwalk", "walkway", "causeway", "gangway", "span",
      "decking", "jetty", "pier", "catwalk"), BRIDGE),
    (("lake", "pool", "river", "stream", "brook", "water", "flood", "fountain",
      "moat", "canal", "shallows", "ford"), WATER),
    (("pit", "chasm", "shaft", "abyss", "sinkhole", "crevasse", "collapsed floor",
      "fallen floor", "broken floor", "missing floor", "weightless"), PIT),
    (("rubble", "debris", "scree", "wreckage", "spoil", "barricade",
      "breastwork", "collapse", "cave-in", "spill"), RUBBLE),
    (("thicket", "undergrowth", "bushes", "hedge", "treeline", "brambles",
      "reeds", "scrub", "briars"), VEGETATION),
)
# Things that stand in a field with room to walk between them.
_ANNOTATION_SPARSE = ("colonnade", "columns", "column", "pillars", "pillar",
                      "obelisk", "boulder", "stalagmite", "menhir", "stump")
# One solid object, small enough to fill the rectangle it was given.
_ANNOTATION_SOLID = ("altar", "dais", "plinth", "pedestal", "throne",
                     "sarcophagus", "tomb", "statue", "anvil", "forge",
                     "brazier stand", "monolith", "idol", "shrine stone")


def _annotation_ground(label):
    """(tile, sparse) for a named region, or (None, False)."""
    low = " " + str(label or "").lower() + " "
    for words, tile in _ANNOTATION_GROUND:
        if any(w in low for w in words):
            return tile, False
    if any(w in low for w in _ANNOTATION_SPARSE):
        return WALL, True
    if any(w in low for w in _ANNOTATION_SOLID):
        return WALL, False
    return None, False


def _apply_annotation_ground(grid, annotations):
    """Lay the ground a region's own name asks for, where nothing else has.

    Only plain floor is written over, so an explicit terrain_zone, a room wall
    and anything the generator meant always win.
    """
    for note in annotations or []:
        try:
            nx, ny = int(note["x"]), int(note["y"])
            nw, nh = max(1, int(note.get("w", 1))), max(1, int(note.get("h", 1)))
        except (KeyError, TypeError, ValueError):
            continue
        tile, sparse = _annotation_ground(note.get("label"))
        if tile is None:
            continue
        # A solid thing that was given half the map is not a solid thing; it is
        # a region that happens to have one in it.
        if tile == WALL and not sparse and nw * nh > 64:
            sparse = True
        for yy in range(ny, ny + nh):
            for xx in range(nx, nx + nw):
                if grid.get(xx, yy) != FLOOR:
                    continue
                if sparse and (xx % 2 or yy % 2):
                    continue
                grid.set(xx, yy, tile)


def _apply_terrain_zones(grid, spec):
    """Terrain the caller placed itself: `terrain_zones: [{kind,x,y,w,h}]`.

    Scattering is fine for atmosphere, but a description that says "a river
    runs down the middle" is giving a rectangle, and it should be able to say
    so rather than hoping the scatter lands there.
    """
    kinds = {"water": WATER, "pit": PIT, "rubble": RUBBLE,
             "vegetation": VEGETATION, "floor": FLOOR, "bridge": BRIDGE,
             "stairs": STAIRS, "wall": WALL, "void": VOID}
    for zone in (spec.get("terrain_zones") or []):
        kind = kinds.get(str(zone.get("kind", "")).lower())
        if kind is None:
            continue
        try:
            x, y = int(zone["x"]), int(zone["y"])
            w, h = int(zone.get("w", 1)), int(zone.get("h", 1))
        except (KeyError, TypeError, ValueError):
            continue
        grid.fill_rect(x, y, w, h, kind)


def _apply_terrain(grid, rooms, spec, rng):
    """Lay water/pits/rubble/undergrowth over already-carved floor."""
    terrain = spec.get("terrain") or {}
    kind = str(terrain.get("kind", "none")).lower()
    if spec.get("layout") == "harbour" and kind in ("water", "sea", "river", "pool"):
        kind = "none"  # the layout already owns the water
    if kind in ("lava", "magma", "acid", "blood", "sea", "river", "pool"):
        kind = WATER  # all liquids render the same; the style prompt colours it
    amount = _AMOUNT_SCALE.get(str(terrain.get("amount", "medium")).lower(), 1.0)
    shape = str(terrain.get("shape", "pools")).lower()

    if kind in (WATER, PIT, RUBBLE, VEGETATION) and amount > 0:
        span = min(grid.cols, grid.rows)
        if shape in ("river", "stream", "channel"):
            py = grid.rows // 2 + rng.randint(-2, 2)
            width = _clamp(int(round(span * 0.045 * amount)), 0, 3)
            phase = rng.uniform(0, 6.28)
            prev = None
            for x in range(grid.cols):
                cy = py + int(round(math.sin(x / 4.5 + phase) * 1.5))
                # Span the gap to the previous column so the channel never
                # breaks into disconnected puddles.
                lo, hi = (cy, cy) if prev is None else (min(prev, cy), max(prev, cy))
                for yy in range(lo - width, hi + width + 1):
                    if grid.get(x, yy) == FLOOR:
                        grid.set(x, yy, kind)
                prev = cy
        else:
            count = _clamp(int(round(2 * amount)) + len(rooms) // 3, 1, 6)
            for _ in range(count):
                if rooms and rng.random() < 0.75:
                    rx, ry, rw, rh = rng.choice(rooms)["rect"]
                    cx = rx + rng.randint(0, max(0, rw - 1))
                    cy = ry + rng.randint(0, max(0, rh - 1))
                else:
                    cx = rng.randint(2, max(2, grid.cols - 3))
                    cy = rng.randint(2, max(2, grid.rows - 3))
                radius = _clamp(span * 0.09 * amount * rng.uniform(0.7, 1.4), 1.2, 6.0)
                _blob(grid, cx, cy, radius, kind, rng, only_on={FLOOR},
                      squish=rng.uniform(0.8, 1.6))

    # Per-area terrain overrides let one chamber be flooded while others are dry.
    for room in rooms:
        rk = str(room["spec"].get("terrain", "none")).lower()
        if rk in ("lava", "magma", "acid", "blood", "pool", "river"):
            rk = WATER
        if rk not in (WATER, PIT, RUBBLE, VEGETATION):
            continue
        rx, ry, rw, rh = room["rect"]
        _blob(grid, rx + rw / 2.0, ry + rh / 2.0, min(rw, rh) * 0.42,
              rk, rng, only_on={FLOOR}, squish=rw / max(1.0, float(rh)))


# --- What closes a site in -------------------------------------------------
# A gorge is not a building and a clearing has no masonry, but until this
# existed every map was described to the renderer as though it were a walled
# house: cliffs became courses of dressed stone and the way in became a timber
# door with iron bands. One rule, used by the architect and by the caption, so
# the plan and the words for it can never disagree.

_ENCLOSURE_KINDS = ("masonry", "rock", "timber", "open")

# Layout decides first, because it is what actually got carved.
_MASONRY_LAYOUTS = ("building", "dungeon", "arena")
_ROCK_LAYOUTS = ("cavern",)
_TIMBER_LAYOUTS = ("deck",)
_OPEN_LAYOUTS = ("open", "forest", "swamp", "ruins", "street", "district", "harbour")
# Then the style, for layouts that could be either - "custom" above all.
_ROCK_CATEGORIES = ("cavern", "underground")
_TIMBER_CATEGORIES = ("nautical",)
_OPEN_CATEGORIES = ("wilderness", "settlement", "urban")

_STYLE_CACHE = {}


def style_data(style_id):
    """The style JSON by id, or an empty dict. Cached."""
    key = str(style_id or "").strip().lower()
    if not key:
        return {}
    if key in _STYLE_CACHE:
        return _STYLE_CACHE[key]
    data = {}
    try:
        from paths import ROOT as _ROOT
        path = _ROOT / "styles" / f"{key}.json"
        if path.exists():
            data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        data = {}
    _STYLE_CACHE[key] = data
    return data


def enclosure_of(style, layout=""):
    """masonry | rock | timber | open - what the outer edge of this site is.

    A style may say so outright with an "enclosure" field. Otherwise the layout
    decides, and for layouts that could be either - "custom" most of all - the
    style's category does.
    """
    if isinstance(style, str) or style is None:
        style = style_data(style)
    named = str(style.get("enclosure", "")).strip().lower()
    if named in _ENCLOSURE_KINDS:
        return named
    layout = str(layout or style.get("default_layout") or "").strip().lower()
    if layout in _MASONRY_LAYOUTS:
        return "masonry"
    if layout in _ROCK_LAYOUTS:
        return "rock"
    if layout in _TIMBER_LAYOUTS:
        return "timber"
    if layout in _OPEN_LAYOUTS:
        return "open"
    category = str(style.get("category", "")).strip().lower()
    if category in _ROCK_CATEGORIES:
        return "rock"
    if category in _TIMBER_CATEGORIES:
        return "timber"
    if category in _OPEN_CATEGORIES:
        return "open"
    return "masonry"


def _ensure_a_way_in(grid, rng, enclosure="masonry"):
    """Every enclosed room gets a way in.

    Connectivity does not catch a sealed room - it is perfectly connected to
    itself. A room nobody can enter is a bug by the same rule that says a map
    must be walkable, so the wall is opened rather than merely complained about.

    What gets cut depends on what the wall is made of. Rock gets a passage,
    because caves have no doors in them; so does the outer edge of an open-air
    site, because you walk into a clearing rather than knocking. A hut standing
    in that clearing still gets a door, since a hut is built.
    """
    for _ in range(12):
        sealed = _enclosed_without_a_door(grid, want_cells=True)
        if not sealed:
            return
        cells = sealed[0]
        inside = set(cells)
        # A wall cell with this room on one side and something else on the other
        # is where a door belongs.
        best = None
        for (x, y) in cells:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                wx, wy = x + dx, y + dy
                if grid.get(wx, wy) != WALL:
                    continue
                ox, oy = wx + dx, wy + dy
                if (ox, oy) in inside:
                    continue
                if not grid.inside(ox, oy):
                    score = 2                      # opens off the edge of the map
                elif grid.get(ox, oy) in WALKABLE:
                    score = 0                      # opens onto somewhere to stand
                elif grid.get(ox, oy) == VOID:
                    score = 1
                else:
                    continue
                if best is None or score < best[0]:
                    best = (score, wx, wy, ox, oy, dx, dy)
        if best is None:
            return
        _, wx, wy, ox, oy, gx, gy = best
        # Does this wall face the outside of the map, or another part of it?
        # A hut standing in a clearing is built and gets a door; the cliff round
        # the clearing is not and gets a gap. Walking outward from the wall
        # answers it: if nothing walkable lies that way, it is the boundary.
        faces_outside = True
        cx, cy = wx, wy
        while grid.inside(cx, cy):
            if grid.get(cx, cy) in WALKABLE and (cx, cy) not in inside:
                faces_outside = False
                break
            cx, cy = cx + gx, cy + gy
        cut_open = enclosure == "rock" or (enclosure == "open" and faces_outside)
        if not cut_open:
            grid.set(wx, wy, DOOR)
            if grid.inside(ox, oy) and grid.get(ox, oy) == VOID:
                grid.set(ox, oy, FLOOR)
            continue
        # Rock and open ground get a passage rather than a door, and the passage
        # is carried right off the edge of the map. A gap that stops inside the
        # cliff is a gap to nowhere: outdoor maps are entered from off the page.
        cx, cy = wx, wy
        while grid.inside(cx, cy):
            if grid.get(cx, cy) not in WALKABLE:
                grid.set(cx, cy, FLOOR)
            cx, cy = cx + gx, cy + gy


def _ensure_connected(grid, rng):
    """If terrain cut the map into islands, bridge them. A battle map you cannot
    walk across is a bug, not a feature.

    Six passes used to be the budget and each pass joined a single cell, so a
    swamp with two dozen reed beds came out with most of them unreachable. It
    now runs until the map is whole, and picks the nearest main cell to one
    stranded cell rather than comparing every pair.
    """
    for _ in range(60):
        main = _largest_component(grid, WALKABLE)
        if not main:
            return
        stranded = None
        for y in range(grid.rows):
            for x in range(grid.cols):
                if grid.get(x, y) in WALKABLE and (x, y) not in main:
                    stranded = (x, y)
                    break
            if stranded:
                break
        if stranded is None:
            return
        target = min(main, key=lambda c: abs(c[0] - stranded[0]) + abs(c[1] - stranded[1]))
        for cell in _carve_corridor(grid, stranded, target, rng):
            here = grid.get(*cell)
            if here in (WATER, PIT, VOID):
                grid.set(cell[0], cell[1], BRIDGE)
            elif here == WALL:
                # _fix_doors runs afterwards and demotes this to a plain opening
                # if it turns out not to be seated in a wall run.
                grid.set(cell[0], cell[1], DOOR)


def _prop_slots(grid, rect, want_wall):
    """Candidate cells for a prop: free floor, either hugging a wall or not."""
    rx, ry, rw, rh = rect
    slots = []
    for y in range(ry - 1, ry + rh + 1):
        for x in range(rx - 1, rx + rw + 1):
            # Outdoor layouts are mostly undergrowth, so restricting placement
            # to bare floor left a forest with three props on it.
            if grid.get(x, y) not in (FLOOR, BRIDGE, VEGETATION, RUBBLE):
                continue
            near_wall = any(grid.get(x + dx, y + dy) == WALL
                            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            near_door = any(grid.get(x + dx, y + dy) == DOOR
                            for dx in (-1, 0, 1) for dy in (-1, 0, 1))
            if near_door:
                continue
            if near_wall == want_wall:
                slots.append((x, y))
    return slots


def _place_props(grid, rooms, spec, rng, style_props=None):
    """Distribute props room by room, respecting what each prop is for."""
    density = _AMOUNT_SCALE.get(str(spec.get("prop_density", "medium")).lower(), 1.0)
    used = set()
    features = []

    def commit(kind, cell, filler=False):
        if cell in used:
            return False
        # Keep a one-cell breathing gap so props stay readable at table scale.
        if any((cell[0] + dx, cell[1] + dy) in used
               for dx in (-1, 0, 1) for dy in (-1, 0, 1)):
            return False
        used.add(cell)
        features.append({"kind": kind, "x": cell[0], "y": cell[1],
                         "structural": is_structural_prop(kind), "filler": bool(filler)})
        return True

    pool = style_props or _DEFAULT_FILLER["dungeon"]

    for room in rooms:
        rect = room["rect"]
        wanted = [normalize_prop(p) for p in (room["spec"].get("props") or [])]
        wanted = [w for w in wanted if w]
        area = max(1, rect[2] * rect[3])
        filler_budget = int(round(area / 22.0 * density))
        # A room that named its own props has said what belongs in it. The
        # style's list says what belongs in this kind of place in general, and
        # using it here is how an abandoned tavern common room came back with a
        # bed and three bookshelves in it - each of which the renderer then
        # built a little side room around.
        room_pool = list(dict.fromkeys(wanted)) or pool
        if room_pool and filler_budget > 0:
            filler_kinds = [rng.choice(room_pool) for _ in range(filler_budget)]
            wanted = wanted + filler_kinds
        else:
            filler_kinds = []
        asked_for = len(wanted) - len(filler_kinds)

        wall_slots = _prop_slots(grid, rect, want_wall=True)
        open_slots = _prop_slots(grid, rect, want_wall=False)
        rng.shuffle(wall_slots)
        rng.shuffle(open_slots)
        centre = _rect_center(rect)
        # Toward the middle, but not in a queue: sorted strictly by distance the
        # props came out as a plus sign of five identical heaps around the exact
        # centre of the room, which is the repeating pattern the caption spends
        # its length warning against.
        spread = max(6.0, (rect[2] + rect[3]) * 0.35)
        open_slots.sort(key=lambda c: abs(c[0] - centre[0]) + abs(c[1] - centre[1])
                        + rng.uniform(0, spread))

        for slot, kind in enumerate(wanted[:14]):
            is_filler = slot >= asked_for
            prefers_wall = kind in WALL_PROPS
            primary = wall_slots if prefers_wall else open_slots
            secondary = open_slots if prefers_wall else wall_slots
            placed = False
            for slots in (primary, secondary):
                for cell in list(slots):
                    if commit(kind, cell, filler=is_filler):
                        slots.remove(cell)
                        placed = True
                        break
                if placed:
                    break

    # Per-room budgets alone leave a big map almost bare - a 62x51 cavern came
    # out with three props - because "rooms" on organic layouts are only small
    # sample points. Top up against the actual walkable area.
    walkable_cells = []
    for y in range(grid.rows):
        for x in range(grid.cols):
            if grid.get(x, y) in (FLOOR, BRIDGE, VEGETATION, RUBBLE):
                walkable_cells.append((x, y))
    target = int(len(walkable_cells) / 26.0 * density)
    target = _clamp(target, 0, 260)
    asked_kinds = list(dict.fromkeys(
        k for room in rooms
        for k in (normalize_prop(p) for p in (room["spec"].get("props") or [])) if k))
    top_pool = asked_kinds or pool
    if top_pool and len(features) < target:
        rng.shuffle(walkable_cells)
        for cell in walkable_cells:
            if len(features) >= target:
                break
            commit(rng.choice(top_pool), cell, filler=True)

    # Explicit features from the caller always win.
    for f in spec.get("features") or []:
        try:
            cell = (int(f["x"]), int(f["y"]))
        except (KeyError, TypeError, ValueError):
            continue
        if grid.inside(*cell):
            used.add(cell)
            kind = str(f.get("kind", "pillar"))
            features.append({"kind": kind, "x": cell[0], "y": cell[1],
                             "structural": bool(f.get("structural",
                                                     is_structural_prop(kind))),
                             "filler": False})
    return features


# -- tiles -> editable rectangles -------------------------------------

def _extract_zones(grid):
    """Greedy rectangle decomposition, so the app can edit the result as a
    handful of shapes instead of thousands of tiles."""
    zones = [{"id": "base", "kind": VOID, "x": 0, "y": 0,
              "w": grid.cols, "h": grid.rows}]
    claimed = [[False] * grid.cols for _ in range(grid.rows)]
    counters = {}
    for kind in PAINT_ORDER[1:]:
        for y in range(grid.rows):
            for x in range(grid.cols):
                if claimed[y][x] or grid.get(x, y) != kind:
                    continue
                w = 0
                while (x + w < grid.cols and not claimed[y][x + w]
                       and grid.get(x + w, y) == kind):
                    w += 1
                h = 1
                while y + h < grid.rows:
                    ok = all(not claimed[y + h][x + i] and grid.get(x + i, y + h) == kind
                             for i in range(w))
                    if not ok:
                        break
                    h += 1
                for yy in range(y, y + h):
                    for xx in range(x, x + w):
                        claimed[yy][xx] = True
                counters[kind] = counters.get(kind, 0) + 1
                zones.append({"id": f"{kind}_{counters[kind]}", "kind": kind,
                              "x": x, "y": y, "w": w, "h": h})
    return zones


def zones_to_grid(map_data):
    """Rasterize rect zones back into a tile grid, in list order.

    Both the architect's output and hand-edited maps go through this, so the
    renderer behaves identically for generated and user-drawn layouts."""
    grid_cfg = map_data.get("grid", {}) or {}
    cols = _clamp(int(grid_cfg.get("cols", 25) or 25), 3, 200)
    rows = _clamp(int(grid_cfg.get("rows", 19) or 19), 3, 200)
    grid = TileGrid(cols, rows, VOID)
    for z in map_data.get("zones", []) or []:
        if not isinstance(z, dict):
            continue
        kind = str(z.get("kind", FLOOR)).lower()
        if kind in ("room", "corridor", "street", "courtyard", "building", "altar"):
            kind = FLOOR  # legacy aliases from earlier map files
        if kind not in TILE_KINDS:
            kind = FLOOR
        try:
            x, y = int(z.get("x", 0)), int(z.get("y", 0))
            w, h = int(z.get("w", 1)), int(z.get("h", 1))
        except (TypeError, ValueError):
            continue
        grid.fill_rect(x, y, max(1, w), max(1, h), kind)
    return grid


# -- spec normalisation and the public entry point --------------------

_DEFAULT_ROOMS = [
    {"id": "main_hall", "label": "Main Hall", "size": "l", "props": [],
     "description": "The largest space, worn smooth down the middle where people walk."},
    {"id": "side_room", "label": "Side Chamber", "size": "m", "props": [],
     "description": "A smaller room off the main one, its floor less worn."},
    {"id": "back_room", "label": "Back Chamber", "size": "m", "props": [],
     "description": "The room furthest from the entrance, dusty and little used."},
]


def _clip_sentence(text, limit):
    """Trim to a length without leaving the sentence hanging."""
    text = text.strip()
    if len(text) <= limit:
        return text
    cut = text[:limit]
    stop = max(cut.rfind(". "), cut.rfind("; "))
    return (cut[:stop + 1] if stop > limit // 2 else cut.rstrip(" ,;")).strip()


def normalize_spec(spec):
    """Accept anything an LLM or agent might plausibly hand us and turn it into
    a spec the generators can rely on."""
    spec = dict(spec or {})
    out = {}

    out["name"] = str(spec.get("name") or spec.get("id") or "battlemap").strip() or "battlemap"
    out["name"] = "".join(c if (c.isalnum() or c in "_-") else "_" for c in out["name"].lower())[:48]
    out["title"] = str(spec.get("title") or spec.get("name") or "Battle Map").strip()
    out["style"] = str(spec.get("style") or spec.get("style_id") or "").strip()

    layout = str(spec.get("layout") or spec.get("layout_type") or "dungeon").lower().strip()
    aliases = {
        "rooms": "dungeon", "dungeon_rooms": "dungeon", "crypt": "dungeon",
        "interior": "building", "house": "building", "tavern": "building",
        "cave": "cavern", "caves": "cavern", "outdoor": "open",
        "field": "open", "meadow": "open", "clearing": "open", "city": "street", "town": "street",
        "boss": "arena", "chamber": "arena", "explicit": "custom",
        "woods": "forest", "woodland": "forest", "jungle": "forest",
        "thicket": "forest", "grove": "forest",
        "marsh": "swamp", "bog": "swamp", "fen": "swamp", "wetland": "swamp",
        "ruin": "ruins", "rubble_field": "ruins", "battlefield": "ruins",
        "port": "harbour", "harbor": "harbour", "docks": "harbour",
        "dock": "harbour", "waterfront": "harbour", "quay": "harbour",
        "city": "district", "town": "district", "quarter": "district",
        "block": "district", "alley": "district", "slum": "district",
        "pier": "harbour", "coast": "harbour",
        "ship": "deck", "shipdeck": "deck", "at_sea": "deck", "sea": "deck",
        "boarding": "deck", "galleon": "deck",
    }
    layout = aliases.get(layout, layout)
    out["layout"] = layout if layout in _GENERATORS else "dungeon"

    grid_cfg = spec.get("grid") or {}
    size_name = str(spec.get("size") or "medium").lower()
    cols = grid_cfg.get("cols") or spec.get("cols")
    rows = grid_cfg.get("rows") or spec.get("rows")
    if not cols or not rows:
        cols, rows = SIZE_PRESETS.get(size_name, SIZE_PRESETS["medium"])
    out["cols"] = _clamp(int(cols), MIN_CELLS, MAX_CELLS)
    out["rows"] = _clamp(int(rows), MIN_CELLS, MAX_CELLS)

    rooms = spec.get("rooms") or spec.get("areas") or []
    clean_rooms = []
    for i, r in enumerate(rooms):
        if isinstance(r, str):
            r = {"label": r}
        if not isinstance(r, dict):
            continue
        label = str(r.get("label") or r.get("name") or r.get("id") or f"Area {i + 1}")
        entry = {
            "id": str(r.get("id") or label).lower().replace(" ", "_")[:32] or f"area_{i}",
            "label": label[:40],
            # What this room is, in the planner's own words. It goes to the
            # renderer with the room's rectangle, so it is worth keeping.
            "description": _clip_sentence(
                str(r.get("description") or r.get("desc") or ""), 600),
            "size": str(r.get("size", "m"))[:1].lower(),
            "props": r.get("props") or r.get("features") or [],
            "terrain": str(r.get("terrain", "none")).lower(),
        }
        for key in ("x", "y", "w", "h"):
            if key in r:
                entry[key] = r[key]
        clean_rooms.append(entry)
    if not clean_rooms:
        clean_rooms = [dict(r) for r in _DEFAULT_ROOMS]
    out["rooms"] = clean_rooms[:9]

    terrain = spec.get("terrain")
    if isinstance(terrain, str):
        terrain = {"kind": terrain, "amount": "medium"}
    out["terrain"] = terrain or {"kind": "none"}
    # Pinned notes and atmosphere travel with the spec so the bleed margin
    # shifts them along with everything else on the map.
    out["annotations"] = [dict(n) for n in (spec.get("annotations") or [])
                          if isinstance(n, dict)]
    out["effects"] = [dict(e) for e in (spec.get("effects") or []) if isinstance(e, dict)]
    out["terrain_zones"] = [z for z in (spec.get("terrain_zones") or [])
                            if isinstance(z, dict)]

    out["prop_density"] = str(spec.get("prop_density", "high")).lower()
    # Callers may widen or disable the blank ring; they may not make it huge.
    out["border"] = _clamp(int(spec.get("border", BORDER_CELLS) or 0), 0, 8)
    # Outdoor sites continue past the frame; indoor ones are enclosed. Keyed on
    # what closes the site in rather than on a list of layout names, because
    # every scene an agent writes uses "custom" and "custom" was not on the
    # list - so a city street came out as a room with walls round it and a
    # fountain square came out as a chamber with a fountain in it.
    out["edge_walls"] = bool(spec.get(
        "edge_walls", enclosure_of(out["style"], out["layout"]) != "open"))
    if "water_fraction" in spec:
        out["water_fraction"] = spec["water_fraction"]
    props = spec.get("style_props") or []
    out["style_props"] = [q for q in (normalize_prop(p) for p in props) if q]
    out["features"] = spec.get("features") or []
    out["scene_summary"] = str(spec.get("scene_summary") or spec.get("summary") or "").strip()
    out["lighting"] = str(spec.get("lighting") or "").strip()
    return out


def _shift_all(map_data, dx, dy):
    """Move every coordinate-bearing item on the map by (dx, dy)."""
    for key in ("zones", "features", "areas", "structures", "annotations",
                "effects", "labels"):
        for item in map_data.get(key, []) or []:
            if "x" in item:
                item["x"] = int(item["x"]) + dx
            if "y" in item:
                item["y"] = int(item["y"]) + dy


def add_border(map_data, cells=BORDER_CELLS):
    """Grow the map by an empty ring of `cells` on every side.

    The playable field keeps exactly the size that was asked for; this is added
    around it. Calling it twice is a no-op, so a map that already carries a
    border can be passed through freely.
    """
    b = max(0, int(cells))
    meta = map_data.setdefault("meta", {})
    if b == 0 or int(meta.get("border", 0) or 0) > 0:
        return map_data
    grid_cfg = map_data.setdefault("grid", {})
    play_cols = int(grid_cfg.get("cols", 0) or 0)
    play_rows = int(grid_cfg.get("rows", 0) or 0)
    if play_cols <= 0 or play_rows <= 0:
        return map_data
    grid_cfg["cols"] = play_cols + 2 * b
    grid_cfg["rows"] = play_rows + 2 * b
    _shift_all(map_data, b, b)
    meta["border"] = b
    return map_data


def border_of(map_data):
    """How wide the empty ring is on this map, in cells."""
    return max(0, int(((map_data or {}).get("meta") or {}).get("border", 0) or 0))


def playable_rect(map_data):
    """(x, y, w, h) of the field the user actually owns."""
    b = border_of(map_data)
    grid_cfg = (map_data or {}).get("grid", {}) or {}
    cols = int(grid_cfg.get("cols", 0) or 0)
    rows = int(grid_cfg.get("rows", 0) or 0)
    return (b, b, max(1, cols - 2 * b), max(1, rows - 2 * b))


def build(spec, seed=None):
    """Turn a semantic scene spec into a complete, valid map."""
    spec = normalize_spec(spec)
    rng = random.Random(seed if seed is not None else random.randrange(1 << 30))

    grid = TileGrid(spec["cols"], spec["rows"], VOID)
    rooms, paths = _GENERATORS[spec["layout"]](grid, spec, rng)
    # Derived first, explicit second: a scene that carved a stone rib across its
    # lake means it, and the lake annotation lying over the same rectangle must
    # not fill the rib back in. Both before the outdoor ground is spread, so
    # that spreading stops at whatever the scene has already put down.
    _apply_annotation_ground(grid, spec.get("annotations"))
    _apply_terrain_zones(grid, spec)
    _open_up_outdoor_rooms(grid, rooms,
                           enclosure_of(spec.get("style"), spec.get("layout")))
    _apply_terrain(grid, rooms, spec, rng)
    _derive_walls(grid)
    if paths:
        _place_doors(grid, rooms, paths)
    _ensure_connected(grid, rng)
    _derive_walls(grid)
    if not spec["edge_walls"]:
        _open_edges(grid)
    # After the walls are final, or the opening would be walled up again.
    _ensure_a_way_in(grid, rng,
                     enclosure=enclosure_of(spec.get("style"), spec.get("layout")))
    _fix_doors(grid)

    filler = spec.get("style_props") or _DEFAULT_FILLER.get(spec["layout"])
    features = _place_props(grid, rooms, spec, rng, style_props=filler)

    map_data = {
        "meta": {
            "name": spec["name"],
            "title": spec["title"],
            "style": spec["style"],
            "layout": spec["layout"],
            "scene_summary": spec["scene_summary"],
            "render_details": spec.get("render_details", ""),
            # The scene's own lighting. Normalised into the spec since the
            # beginning and then dropped on the floor here, so a description
            # that said "lit only by the fire" was painted in whatever the
            # style felt like.
            "lighting": spec.get("lighting", ""),
            # Kept so opening a plan restores the settings it was built with.
            "terrain_kind": str((spec.get("terrain") or {}).get("kind", "none")).lower(),
            "terrain_amount": str((spec.get("terrain") or {}).get("amount", "medium")).lower(),
            "prop_density": spec["prop_density"],
            "seed": seed,
        },
        "grid": {
            "cols": grid.cols,
            "rows": grid.rows,
            "cell_px": 32,
        },
        "zones": _extract_zones(grid),
        "structures": spec.get("structures") or [],
        "features": features,
        # Labels are metadata for the human-facing preview only. They are never
        # drawn into the image sent to the diffusion model - that is exactly
        # what used to produce garbled lettering all over the final map.
        "labels": [],
        "areas": [],
        # Hand-written notes pinned to a rectangle. They outrank everything else
        # in the caption, because the user placed them deliberately.
        "annotations": [dict(n) for n in (spec.get("annotations") or [])],
        # Atmospheric overlays. A separate top layer: they never alter the
        # ground or block movement.
        "effects": [dict(e) for e in (spec.get("effects") or [])],
    }
    for room in rooms:
        rx, ry, rw, rh = room["rect"]
        label = room["spec"].get("label", "")
        map_data["areas"].append({"id": room["spec"].get("id", ""), "label": label,
                                  "description": room["spec"].get("description", ""),
                                  "x": rx, "y": ry, "w": rw, "h": rh})
        if label:
            map_data["labels"].append({"text": label, "x": rx + rw // 2,
                                       "y": ry + rh // 2, "size": "md"})
    # The blank ring goes on last, so every generator above keeps working in
    # plain 0..cols coordinates and knows nothing about it.
    add_border(map_data, int(spec.get("border", BORDER_CELLS)))
    return map_data


def _enclosed_without_a_door(grid, want_cells=False, margin=0):
    """Rooms you cannot get into, because nothing opens through their wall.

    Connectivity does not catch this: a sealed room is perfectly connected to
    itself. A map whose only building has no door in it is a plan somebody
    forgot to finish, and it is worth saying so before the GPU is spent.
    """
    seen = [[False] * grid.cols for _ in range(grid.rows)]
    sealed = []
    for sy in range(grid.rows):
        for sx in range(grid.cols):
            if seen[sy][sx] or grid.get(sx, sy) not in WALKABLE:
                continue
            stack = [(sx, sy)]
            seen[sy][sx] = True
            cells = []
            touches_edge = False
            has_opening = False
            while stack:
                x, y = stack.pop()
                cells.append((x, y))
                # The playable field, not the stored grid: a map carries an
                # empty bleed margin around it, so ground that runs off the
                # field is ground that runs off the map.
                if (x <= margin or y <= margin or x >= grid.cols - 1 - margin
                        or y >= grid.rows - 1 - margin):
                    touches_edge = True
                # A door only counts as a way in if it leads out of this
                # region. Every door in a fortress is a door, but if all of
                # them are between one chamber and the next then the fortress
                # has no entrance, and nothing used to notice.
                if grid.get(x, y) in (DOOR, WINDOW):
                    for ddx, ddy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        ax, ay = x + ddx, y + ddy
                        if not grid.inside(ax, ay) or grid.get(ax, ay) == VOID:
                            has_opening = True
                            break
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if (0 <= nx < grid.cols and 0 <= ny < grid.rows and not seen[ny][nx]
                            and grid.get(nx, ny) in WALKABLE):
                        seen[ny][nx] = True
                        stack.append((nx, ny))
            # An outdoor scene runs off the edge of the map and needs no door.
            if not touches_edge and not has_opening and len(cells) >= 12:
                sealed.append(cells if want_cells else len(cells))
    return sealed


def validate_map(map_data, repair=True):
    """Check (and optionally fix) a map that came from an agent, a file or the
    editor. Returns (map_data, list_of_problems)."""
    problems = []
    data = dict(map_data or {})

    # A placeholder name is worse than no name: it is sent to the artist as if
    # it meant something. Worth saying out loud rather than quietly rendering.
    import re as _re
    placeholder = _re.compile(r"^(area|room|zone|space|section|building)\s*\d*$|"
                              r"^(main|second|third|fourth|fifth|another)\s+(area|room|zone)$",
                              _re.I)
    seen_labels = {}
    for a in (data.get("areas") or []):
        label = str(a.get("label", "")).strip()
        if not label:
            problems.append("an area has no name, so the renderer is told nothing about it")
        elif placeholder.match(label):
            problems.append(f"area '{label}' still has a placeholder name - "
                            f"name it for what happens there")
        seen_labels[label.lower()] = seen_labels.get(label.lower(), 0) + 1
        if not str(a.get("description", "")).strip():
            problems.append(f"area '{label or '?'}' has no description, so only its name "
                            f"reaches the renderer")
    for label, n in seen_labels.items():
        if n > 1 and label:
            problems.append(f"{n} areas are all called '{label}' - "
                            f"the renderer cannot tell them apart")

    meta = data.get("meta")
    if not isinstance(meta, dict):
        meta, problems = {}, problems + ["meta missing"]
    meta.setdefault("name", "battlemap")
    meta.setdefault("title", "Battle Map")
    meta.setdefault("style", "")
    meta.setdefault("layout", "dungeon")
    meta.setdefault("scene_summary", "")
    meta.setdefault("render_details", "")
    meta.setdefault("lighting", "")
    meta.setdefault("terrain_kind", "none")
    meta.setdefault("terrain_amount", "medium")
    meta.setdefault("prop_density", "high")
    data["meta"] = meta

    grid_cfg = data.get("grid") if isinstance(data.get("grid"), dict) else {}
    # The stored grid includes the blank ring, so it may legitimately exceed the
    # limit the user is allowed to pick.
    hard_max = MAX_CELLS + 2 * 8
    cols = _clamp(int(grid_cfg.get("cols", 25) or 25), MIN_CELLS, hard_max)
    rows = _clamp(int(grid_cfg.get("rows", 19) or 19), MIN_CELLS, hard_max)
    if (grid_cfg.get("cols"), grid_cfg.get("rows")) != (cols, rows):
        problems.append("grid size clamped")
    data["grid"] = {
        "cols": cols,
        "rows": rows,
        "cell_px": _clamp(int(grid_cfg.get("cell_px", 32) or 32), 8, 128),
    }

    zones = []
    for i, z in enumerate(data.get("zones") or []):
        if not isinstance(z, dict):
            problems.append(f"zone {i} is not an object")
            continue
        kind = str(z.get("kind", FLOOR)).lower()
        if kind not in TILE_KINDS:
            problems.append(f"zone {i}: unknown kind '{kind}' -> floor")
            kind = FLOOR
        x = _clamp(int(z.get("x", 0)), 0, cols - 1)
        y = _clamp(int(z.get("y", 0)), 0, rows - 1)
        w = _clamp(int(z.get("w", 1)), 1, cols - x)
        h = _clamp(int(z.get("h", 1)), 1, rows - y)
        entry = {"id": str(z.get("id") or f"{kind}_{i}"), "kind": kind,
                 "x": x, "y": y, "w": w, "h": h}
        if z.get("dir"):
            entry["dir"] = str(z["dir"])
        zones.append(entry)
    if not zones:
        problems.append("no zones - inserting an empty room")
        zones = [{"id": "base", "kind": VOID, "x": 0, "y": 0, "w": cols, "h": rows},
                 {"id": "floor_1", "kind": FLOOR, "x": 1, "y": 1,
                  "w": cols - 2, "h": rows - 2}]
    elif zones[0]["kind"] != VOID or zones[0]["w"] < cols or zones[0]["h"] < rows:
        zones.insert(0, {"id": "base", "kind": VOID, "x": 0, "y": 0,
                         "w": cols, "h": rows})
    data["zones"] = zones

    feats = []
    for f in data.get("features") or []:
        if not isinstance(f, dict):
            continue
        kind = str(f.get("kind", "pillar")).lower().replace(" ", "_")
        label = str(f.get("label", "")).strip()
        entry = {"kind": kind,
                 "x": _clamp(int(f.get("x", 0)), 0, cols - 1),
                 "y": _clamp(int(f.get("y", 0)), 0, rows - 1),
                 # A custom prop is always pinned: the user placed it deliberately.
                 "structural": bool(f.get("structural",
                                          is_structural_prop(kind) or bool(label)))}
        if label:
            entry["label"] = label
            entry["description"] = str(f.get("description", "")).strip()
            entry["elaboration"] = str(f.get("elaboration", "some")).lower()
        feats.append(entry)
    data["features"] = feats

    labels = []
    for lb in data.get("labels") or []:
        if not isinstance(lb, dict) or not str(lb.get("text", "")).strip():
            continue
        labels.append({"text": str(lb["text"]),
                       "x": _clamp(int(lb.get("x", 0)), 0, cols - 1),
                       "y": _clamp(int(lb.get("y", 0)), 0, rows - 1),
                       "size": str(lb.get("size", "md"))})
    data["labels"] = labels
    data.setdefault("areas", [])

    annotations = []
    for a in data.get("annotations") or []:
        if not isinstance(a, dict) or not str(a.get("label", "")).strip():
            continue
        x = _clamp(int(a.get("x", 0)), 0, cols - 1)
        y = _clamp(int(a.get("y", 0)), 0, rows - 1)
        annotations.append({"label": str(a["label"]).strip(),
                            "description": str(a.get("description", "")).strip(),
                            "elaboration": str(a.get("elaboration", "some")).lower(),
                            "x": x, "y": y,
                            "w": _clamp(int(a.get("w", 1)), 1, cols - x),
                            "h": _clamp(int(a.get("h", 1)), 1, rows - y)})
    data["annotations"] = annotations

    effects = []
    for e in data.get("effects") or []:
        if not isinstance(e, dict):
            continue
        kind = str(e.get("kind", "")).lower().replace(" ", "_")
        label = str(e.get("label", "")).strip()
        if not kind and not label:
            continue
        x = _clamp(int(e.get("x", 0)), 0, cols - 1)
        y = _clamp(int(e.get("y", 0)), 0, rows - 1)
        effects.append({"kind": kind or "custom", "label": label,
                        "description": str(e.get("description", "")).strip(),
                        "elaboration": str(e.get("elaboration", "some")).lower(),
                        "intensity": str(e.get("intensity", "medium")).lower(),
                        "x": x, "y": y,
                        "w": _clamp(int(e.get("w", 1)), 1, cols - x),
                        "h": _clamp(int(e.get("h", 1)), 1, rows - y)})
    data["effects"] = effects

    if repair:
        grid = zones_to_grid(data)

    for size in _enclosed_without_a_door(grid, margin=border_of(data)):
        problems.append(f"a walled area of {size} squares has no door or window anywhere in "
                        f"its wall, so there is no way into it")
        walkable = sum(grid.count(k) for k in WALKABLE)
        if walkable < 12:
            problems.append("map has almost no walkable space")
    return data, problems


if __name__ == "__main__":
    import json
    demo = {
        "name": "flooded_crypt", "title": "Flooded Crypt", "layout": "dungeon",
        "size": "medium", "terrain": {"kind": "water", "amount": "medium"},
        "rooms": [
            {"label": "Entry Hall", "size": "m", "props": ["torch", "rubble"]},
            {"label": "Nave", "size": "l", "props": ["altar", "brazier", "pillar"]},
            {"label": "Ossuary", "size": "m", "props": ["sarcophagus", "chest"]},
        ],
    }
    print(json.dumps(build(demo, seed=7), indent=2)[:2000])
