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

SIZE_PRESETS = {
    "small": (17, 13),
    "medium": (25, 19),
    "large": (66, 50),
    "huge": (100, 75),
    "giant": (150, 150),
}

LAYOUTS = ["dungeon", "building", "cavern", "open", "forest", "swamp", "ruins",
           "street", "arena", "harbour", "custom"]

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


def is_structural_prop(kind):
    """Should this prop get its own bounding box, or be left to the renderer?"""
    return str(kind).lower() in STRUCTURAL_PROPS


def normalize_prop(raw):
    """Map whatever the planner called a prop onto a kind we can actually draw.

    Language models write "wooden_barrels" and "mooring_bollards"; without this
    every such prop degrades to a featureless blob."""
    name = str(raw).strip().lower().replace(" ", "_").replace("-", "_")
    if not name:
        return ""
    if name in KNOWN_PROPS:
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
    """Cellular-automata cave - organic, no straight lines."""
    for y in range(grid.rows):
        for x in range(grid.cols):
            edge = x <= 1 or y <= 1 or x >= grid.cols - 2 or y >= grid.rows - 2
            grid.set(x, y, VOID if edge or rng.random() < 0.44 else FLOOR)
    for _ in range(5):
        snapshot = [row[:] for row in grid.cells]
        for y in range(1, grid.rows - 1):
            for x in range(1, grid.cols - 1):
                walls = 0
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        if dx == 0 and dy == 0:
                            continue
                        yy, xx = y + dy, x + dx
                        if not grid.inside(xx, yy) or snapshot[yy][xx] == VOID:
                            walls += 1
                grid.set(x, y, VOID if walls >= 5 else FLOOR)

    keep = _largest_component(grid, {FLOOR})
    # A cave that collapsed to nothing is useless - fall back to a big chamber.
    if len(keep) < (grid.cols * grid.rows) // 6:
        grid.fill_rect(0, 0, grid.cols, grid.rows, VOID)
        _blob(grid, grid.cols / 2, grid.rows / 2, min(grid.cols, grid.rows) / 2.4,
              FLOOR, rng, squish=1.4)
        keep = _largest_component(grid, {FLOOR})
    for y in range(grid.rows):
        for x in range(grid.cols):
            if grid.get(x, y) == FLOOR and (x, y) not in keep:
                grid.set(x, y, VOID)

    # Treat wide lobes of the cave as "rooms" so props still land sensibly.
    rooms = []
    cells = sorted(keep)
    if cells:
        n = _clamp(len(spec["rooms"]), 1, 6)
        step = max(1, len(cells) // n)
        for i in range(n):
            cx, cy = cells[min(len(cells) - 1, i * step + step // 2)]
            rooms.append({"spec": spec["rooms"][i % len(spec["rooms"])],
                          "rect": (max(0, cx - 2), max(0, cy - 2), 5, 5)})
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
        for i in range(per_side):
            idx = side * per_side + i
            if idx >= n:
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
    return rooms, []


def _gen_arena(grid, spec, rng):
    """One dramatic chamber - the classic boss room."""
    m = 2
    grid.fill_rect(m, m, grid.cols - m * 2, grid.rows - m * 2, FLOOR)
    rect = (m + 1, m + 1, grid.cols - m * 2 - 2, grid.rows - m * 2 - 2)
    specs = spec["rooms"] or [{"id": "arena", "label": "Arena", "props": []}]
    rooms = [{"spec": specs[0], "rect": rect}]
    if grid.cols >= 15 and grid.rows >= 11:
        for gx in (m + 3, grid.cols - m - 4):
            for gy in range(m + 3, grid.rows - m - 3, 3):
                grid.set(gx, gy, WALL)
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


def _ensure_connected(grid, rng):
    """If terrain cut the map in two, bridge the gap. A battle map you cannot
    walk across is a bug, not a feature."""
    for _ in range(6):
        main = _largest_component(grid, WALKABLE)
        stranded = set()
        for y in range(grid.rows):
            for x in range(grid.cols):
                if grid.get(x, y) in WALKABLE and (x, y) not in main:
                    stranded.add((x, y))
        if not stranded or not main:
            return
        # Bridge from the stranded cell closest to the main region.
        best = None
        for sx, sy in stranded:
            for mx, my in main:
                d = abs(sx - mx) + abs(sy - my)
                if best is None or d < best[0]:
                    best = (d, (sx, sy), (mx, my))
        if best is None:
            return
        _, a, b = best
        for cell in _carve_corridor(grid, a, b, rng):
            if grid.get(*cell) in (WATER, PIT):
                grid.set(cell[0], cell[1], BRIDGE)


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

    def commit(kind, cell):
        if cell in used:
            return False
        # Keep a one-cell breathing gap so props stay readable at table scale.
        if any((cell[0] + dx, cell[1] + dy) in used
               for dx in (-1, 0, 1) for dy in (-1, 0, 1)):
            return False
        used.add(cell)
        features.append({"kind": kind, "x": cell[0], "y": cell[1],
                         "structural": is_structural_prop(kind)})
        return True

    for room in rooms:
        rect = room["rect"]
        wanted = [normalize_prop(p) for p in (room["spec"].get("props") or [])]
        wanted = [w for w in wanted if w]
        area = max(1, rect[2] * rect[3])
        filler_budget = int(round(area / 22.0 * density))
        pool = style_props or _DEFAULT_FILLER["dungeon"]
        if pool and filler_budget > 0:
            wanted = wanted + [rng.choice(pool) for _ in range(filler_budget)]

        wall_slots = _prop_slots(grid, rect, want_wall=True)
        open_slots = _prop_slots(grid, rect, want_wall=False)
        rng.shuffle(wall_slots)
        rng.shuffle(open_slots)
        centre = _rect_center(rect)
        open_slots.sort(key=lambda c: abs(c[0] - centre[0]) + abs(c[1] - centre[1]))

        for kind in wanted[:14]:
            prefers_wall = kind in WALL_PROPS
            primary = wall_slots if prefers_wall else open_slots
            secondary = open_slots if prefers_wall else wall_slots
            placed = False
            for pool in (primary, secondary):
                for cell in list(pool):
                    if commit(kind, cell):
                        pool.remove(cell)
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
    if pool and len(features) < target:
        rng.shuffle(walkable_cells)
        for cell in walkable_cells:
            if len(features) >= target:
                break
            commit(rng.choice(pool), cell)

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
                                                     is_structural_prop(kind)))})
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
    {"id": "main_hall", "label": "Main Hall", "size": "l", "props": []},
    {"id": "side_room", "label": "Side Chamber", "size": "m", "props": []},
    {"id": "back_room", "label": "Back Chamber", "size": "m", "props": []},
]


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
        "pier": "harbour", "coast": "harbour", "ship": "harbour",
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

    out["prop_density"] = str(spec.get("prop_density", "high")).lower()
    # Outdoor layouts continue past the frame; indoor ones are enclosed.
    out["edge_walls"] = bool(spec.get(
        "edge_walls",
        out["layout"] not in ("harbour", "open", "street", "forest", "swamp", "ruins")))
    if "water_fraction" in spec:
        out["water_fraction"] = spec["water_fraction"]
    props = spec.get("style_props") or []
    out["style_props"] = [q for q in (normalize_prop(p) for p in props) if q]
    out["features"] = spec.get("features") or []
    out["scene_summary"] = str(spec.get("scene_summary") or spec.get("summary") or "").strip()
    out["lighting"] = str(spec.get("lighting") or "").strip()
    return out


def build(spec, seed=None):
    """Turn a semantic scene spec into a complete, valid map."""
    spec = normalize_spec(spec)
    rng = random.Random(seed if seed is not None else random.randrange(1 << 30))

    grid = TileGrid(spec["cols"], spec["rows"], VOID)
    rooms, paths = _GENERATORS[spec["layout"]](grid, spec, rng)

    _apply_terrain(grid, rooms, spec, rng)
    _derive_walls(grid)
    if paths:
        _place_doors(grid, rooms, paths)
    _ensure_connected(grid, rng)
    _derive_walls(grid)
    if not spec["edge_walls"]:
        _open_edges(grid)
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
        "annotations": [],
        # Atmospheric overlays. A separate top layer: they never alter the
        # ground or block movement.
        "effects": [],
    }
    for room in rooms:
        rx, ry, rw, rh = room["rect"]
        label = room["spec"].get("label", "")
        map_data["areas"].append({"id": room["spec"].get("id", ""), "label": label,
                                  "x": rx, "y": ry, "w": rw, "h": rh})
        if label:
            map_data["labels"].append({"text": label, "x": rx + rw // 2,
                                       "y": ry + rh // 2, "size": "md"})
    return map_data


def validate_map(map_data, repair=True):
    """Check (and optionally fix) a map that came from an agent, a file or the
    editor. Returns (map_data, list_of_problems)."""
    problems = []
    data = dict(map_data or {})

    meta = data.get("meta")
    if not isinstance(meta, dict):
        meta, problems = {}, problems + ["meta missing"]
    meta.setdefault("name", "battlemap")
    meta.setdefault("title", "Battle Map")
    meta.setdefault("style", "")
    meta.setdefault("layout", "dungeon")
    meta.setdefault("scene_summary", "")
    meta.setdefault("render_details", "")
    meta.setdefault("terrain_kind", "none")
    meta.setdefault("terrain_amount", "medium")
    meta.setdefault("prop_density", "high")
    data["meta"] = meta

    grid_cfg = data.get("grid") if isinstance(data.get("grid"), dict) else {}
    cols = _clamp(int(grid_cfg.get("cols", 25) or 25), MIN_CELLS, MAX_CELLS)
    rows = _clamp(int(grid_cfg.get("rows", 19) or 19), MIN_CELLS, MAX_CELLS)
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
