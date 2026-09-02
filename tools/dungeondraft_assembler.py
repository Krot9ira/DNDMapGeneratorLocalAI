#!/usr/bin/env python3
"""Dungeondraft Map Assembler.

Assembles a map plan (from architect.py / agent_api.py) into a native
.dungeondraft_map JSON file, populating tiles, terrain splatmaps, walls,
portals, placed objects, and light sources with matched assets from assets.db.
"""
import datetime
import json
import math
import os
import random
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

from architect import (
    BRIDGE, DOOR, FLOOR, PIT, RUBBLE, STAIRS, VEGETATION, WALL, WATER, WINDOW,
    zones_to_grid,
)
from dungeondraft_matcher import DungeondraftMatcher, DEFAULT_STOCK_LIGHT
from dungeondraft_db import DB_PATH_DEFAULT


def format_vector2(x: float, y: float) -> str:
    """Format a Godot Vector2 string with Dungeondraft spacing."""
    return f"Vector2( {x:.3f}, {y:.3f} )".replace(".000,", ",").replace(".000 )", " )")


def format_pool_vector2_array(points: List[Tuple[float, float]]) -> str:
    """Format a flat x, y coordinate list as a Godot PoolVector2Array string."""
    flat = []
    for x, y in points:
        x_str = f"{x:.2f}".rstrip("0").rstrip(".") if "." in f"{x:.2f}" else f"{x}"
        y_str = f"{y:.2f}".rstrip("0").rstrip(".") if "." in f"{y:.2f}" else f"{y}"
        flat.extend([x_str, y_str])
    return f"PoolVector2Array( {', '.join(flat)} )"


def format_pool_int_array(values: List[int]) -> str:
    """Format an integer list as a Godot PoolIntArray string."""
    return f"PoolIntArray( {', '.join(str(v) for v in values)} )"


def format_pool_byte_array(values: bytes) -> str:
    """Format a byte array as a Godot PoolByteArray string."""
    return f"PoolByteArray( {', '.join(str(b) for b in values)} )"


PROP_OVERSIZE_TOLERANCE = 1.5   # how much bigger than its slot a prop may be
                                # before it is scaled down to fit
DOOR_SNAP_PX = 384          # 1.5 cells: how far a doorway may sit from the
                            # wall line it belongs to and still be cut into it.
CELL_PX = 256.0             # one grid square, in authored pixels

# Dungeondraft's own water defaults. blend_distance is measured in grid cells,
# not pixels: a value in the hundreds makes the shore blend swallow the whole
# body, and the water then renders as one flat slab with hard edges.
WATER_DEEP_COLOR = "ff3aa19a"
WATER_SHALLOW_COLOR = "ff8bceb0"
WATER_BLEND_DISTANCE = 1.5

# Tiles a cave bitmap counts as open floor.
CAVE_FLOOR_KINDS = {FLOOR, BRIDGE, DOOR, WINDOW, STAIRS, RUBBLE, VEGETATION}

# Ground a wall is built to enclose. Water is not on the list on purpose: a
# hull's wall belongs against its deck, not against the harbour outside it.
ENCLOSED_KINDS = {FLOOR, BRIDGE, DOOR, WINDOW, STAIRS, RUBBLE, VEGETATION}

# Which terrain slot each kind of ground paints itself with. The splat's four
# channels are the weights of terrain textures 1-4, and match_terrain_textures
# fills those slots as (paving, greenery, bare earth, silt) - so a marsh stops
# coming out as the same flat stone field as a courtyard.
TERRAIN_CHANNEL_BY_KIND = {
    VEGETATION: 1,
    RUBBLE: 2,
    PIT: 2,
    WATER: 3,
}

DEFAULT_TILES_LOOKUP = {
    "0": "res://textures/tilesets/smart/tileset_wood_vertical.png",
    "1": "res://textures/tilesets/smart_double/tileset_stone.png",
    "2": "res://textures/tilesets/simple/tileset_cave.png",
    "3": "res://textures/tilesets/simple/tileset_brick_running.png",
    "4": "res://textures/tilesets/simple/tileset_cobble.png",
    "5": "res://textures/tilesets/simple/tileset_concrete.png",
    "6": "res://textures/tilesets/simple/tileset_brick_worn.png",
    "7": "res://textures/tilesets/simple/tileset_carpet.png",
    "8": "res://textures/tilesets/simple/tileset_concrete_slab.png",
    "9": "res://textures/tilesets/simple/tileset_concrete_large.png",
    "10": "res://textures/tilesets/simple/tileset_brick_basketweave.png",
    "11": "res://textures/tilesets/simple/tileset_cut_stone.png",
    "12": "res://textures/tilesets/simple/tileset_diamond.png",
    "13": "res://textures/tilesets/simple/tileset_plank_45.png",
    "14": "res://textures/tilesets/simple/tileset_herringbone.png",
    "15": "res://textures/tilesets/simple/tileset_ornate.png",
    "16": "res://textures/tilesets/simple/tileset_diamond_2.png",
    "17": "res://textures/tilesets/simple/tileset_roman_worn.png",
    "18": "res://textures/tilesets/simple/tileset_sewers.png",
    "19": "res://textures/tilesets/simple/tileset_straw.png",
    "20": "res://textures/tilesets/simple/tileset_wood_damaged.png",
    "21": "res://textures/tilesets/simple/tileset_worn_cobble.png",
    "22": "res://textures/tilesets/simple/tileset_wood_interlaced.png",
    "23": "res://textures/tilesets/simple/tileset_roman_tiles.png",
}

DEFAULT_COLOR_PALETTES = {
    "object_custom_colors": [
        "ff6b3834", "ffac584c", "ff885848", "ffc0866c", "ff8d6d58", "fff3a768", "ff685848", "ff9c8868",
        "ffae9254", "ffd8c888", "ff888868", "ffaab478", "ff92aa58", "ff87a868", "ff679865", "ff789868",
        "ff546d56", "ff68887c", "ff667878", "ff809dab", "ff61788d", "ff535869", "ff786878", "ff886878",
        "ff905868", "ff994858", "ffd8d8d8", "ff8a8a8a", "ff585858", "ff282828"
    ],
    "scatter_custom_colors": [
        "ff6b3834", "ffac584c", "ff885848", "ffc0866c", "ff8d6d58", "fff3a768", "ff685848", "ff9c8868",
        "ffae9254", "ffd8c888", "ff888868", "ffaab478", "ff92aa58", "ff87a868", "ff679865", "ff789868",
        "ff546d56", "ff68887c", "ff667878", "ff809dab", "ff61788d", "ff535869", "ff786878", "ff886878",
        "ff905868", "ff994858", "ffd8d8d8", "ff8a8a8a", "ff585858", "ff282828"
    ],
    "light_colors": ["ffeccd8b", "ffeaefca", "ff80beff", "ffffad58", "ff4dd569"],
    "grid_colors": ["7fffffff", "7fcccccc", "7f333333", "7f000000"],
    "deep_water_colors": ["ff3aa19a", "ff8bceb0", "ffffcc55", "ff3c8ab8"],
    "shallow_water_colors": ["ff3ac3b2", "ff8bceb0", "ffcc5555", "ff54c1da"],
    "cave_ground_colors": ["ff7f7e71"],
    "cave_wall_colors": ["ff7f7e71"],
}


def nearest_point_on_polyline(
    points: List[Tuple[float, float]], loop: bool, x: float, y: float
):
    """Closest point on a wall polyline to (x, y).

    Returns (distance, segment_index, t, px, py, rotation, direction) where t is
    the parametric position along that segment - which is exactly what a portal
    stores as point_index and wall_distance.
    """
    best = None
    n = len(points)
    if n < 2:
        return None
    last = n if loop else n - 1
    for i in range(last):
        ax, ay = points[i]
        bx, by = points[(i + 1) % n]
        dx, dy = bx - ax, by - ay
        seg_len_sq = dx * dx + dy * dy
        if seg_len_sq <= 0:
            continue
        t = ((x - ax) * dx + (y - ay) * dy) / seg_len_sq
        t = max(0.0, min(1.0, t))
        px, py = ax + t * dx, ay + t * dy
        dist = math.hypot(x - px, y - py)
        if best is None or dist < best[0]:
            seg_len = math.sqrt(seg_len_sq)
            best = (dist, i, t, px, py, math.atan2(dy, dx), (dx / seg_len, dy / seg_len))
    return best


def shift_items(items: List[dict], delta: int) -> List[dict]:
    """Copy plan items with their coordinates moved, leaving the plan untouched."""
    moved = []
    for item in items or []:
        if not isinstance(item, dict):
            continue
        copy = dict(item)
        if "x" in copy:
            copy["x"] = int(copy["x"]) + delta
        if "y" in copy:
            copy["y"] = int(copy["y"]) + delta
        moved.append(copy)
    return moved


def cells_of_kind(grid, kinds: Set[str]) -> Set[Tuple[int, int]]:
    """Every cell of the rasterized plan whose tile kind is one of kinds."""
    return {(x, y)
            for y in range(grid.rows)
            for x in range(grid.cols)
            if grid.get(x, y) in kinds}


ORTHOGONAL = ((1, 0), (-1, 0), (0, 1), (0, -1))
DIAGONAL = ((1, 1), (1, -1), (-1, 1), (-1, -1))


def connected_cell_groups(cells: Set[Tuple[int, int]],
                          diagonal: bool = False) -> List[Set[Tuple[int, int]]]:
    """Split a cell set into connected components.

    Walls need the diagonal pass: a hull or a slanted run steps corner to
    corner, and splitting it there is what leaves a ship drawn as three
    unrelated pieces of planking.
    """
    steps = ORTHOGONAL + DIAGONAL if diagonal else ORTHOGONAL
    remaining = set(cells)
    groups = []
    while remaining:
        start = remaining.pop()
        group = {start}
        stack = [start]
        while stack:
            x, y = stack.pop()
            for dx, dy in steps:
                n = (x + dx, y + dy)
                if n in remaining:
                    remaining.discard(n)
                    group.add(n)
                    stack.append(n)
        groups.append(group)
    return groups


def cell_centre(x: int, y: int) -> Tuple[float, float]:
    """Pixel centre of a grid cell - where a wall line through it runs."""
    return ((x + 0.5) * CELL_PX, (y + 0.5) * CELL_PX)


def simplify_polyline(points: List[Tuple[float, float]], loop: bool) -> List[Tuple[float, float]]:
    """Drop points that sit on a straight run between their two neighbours."""
    n = len(points)
    if n < 3:
        return list(points)
    kept = []
    for i in (range(n) if loop else range(1, n - 1)):
        ax, ay = points[(i - 1) % n]
        bx, by = points[i]
        cx, cy = points[(i + 1) % n]
        if abs((bx - ax) * (cy - by) - (by - ay) * (cx - bx)) > 1e-6:
            kept.append(points[i])
    if loop:
        return kept if len(kept) >= 3 else list(points)
    return [points[0]] + kept + [points[-1]]


def cell_box(cell: Tuple[int, int]) -> List[Tuple[float, float]]:
    """The four corners of one cell, so a lone wall tile still reads as a pillar."""
    x, y = cell
    return [(x * CELL_PX, y * CELL_PX), ((x + 1) * CELL_PX, y * CELL_PX),
            ((x + 1) * CELL_PX, (y + 1) * CELL_PX), (x * CELL_PX, (y + 1) * CELL_PX)]


def is_thin_region(cells: Set[Tuple[int, int]]) -> bool:
    """True when a wall region is a one-cell-thick run rather than a solid mass.

    A run of wall tiles is a line and wants a centreline; a block of them is a
    rock face and wants its outline. Skeletonising a block instead produces one
    stub per tile, which is how a cavern ends up with hundreds of walls in it.
    """
    return not any((x + 1, y) in cells and (x, y + 1) in cells and (x + 1, y + 1) in cells
                   for (x, y) in cells)


def trace_wall_polylines(cells: Set[Tuple[int, int]]) -> List[Tuple[List[Tuple[float, float]], bool]]:
    """Chain one-cell-thick wall tiles into polylines through their centres.

    A plan describes walls as tiles; Dungeondraft wants polylines. Walking the
    cell graph is what makes corners actually meet: converting each wall
    rectangle on its own puts the horizontal run on one centreline and the
    vertical run on another, which leaves a half-cell hole at every corner and
    a half-cell stub past every end - a map of disconnected floating walls.
    """
    adj: Dict[Tuple[int, int], Set[Tuple[int, int]]] = {c: set() for c in cells}
    for (x, y) in cells:
        for n in ((x + 1, y), (x, y + 1)):
            if n in cells:
                adj[(x, y)].add(n)
                adj[n].add((x, y))
    # A ship hull or a slanted wall steps diagonally. Bridge that step, but only
    # where there is no orthogonal way round it, so solid blocks stay clean.
    for (x, y) in cells:
        for dx, dy in ((1, 1), (1, -1)):
            n = (x + dx, y + dy)
            if n in cells and (x + dx, y) not in cells and (x, y + dy) not in cells:
                adj[(x, y)].add(n)
                adj[n].add((x, y))

    used: Set[frozenset] = set()

    def walk(start, first):
        """Follow one run of wall from start, carrying straight through
        junctions. Stopping at every T instead would chop a long wall into a
        two-point stub per branch, which is a lot of walls saying very little."""
        path = [start, first]
        used.add(frozenset((start, first)))
        cur = first
        heading = (first[0] - start[0], first[1] - start[1])
        while True:
            onward = sorted(n for n in adj[cur] if frozenset((cur, n)) not in used)
            if not onward:
                break
            if len(adj[cur]) == 2:
                step = onward[0]
            else:
                ahead = [n for n in onward
                         if (n[0] - cur[0], n[1] - cur[1]) == heading]
                if not ahead:
                    break
                step = ahead[0]
            used.add(frozenset((cur, step)))
            path.append(step)
            heading = (step[0] - cur[0], step[1] - cur[1])
            cur = step
            if cur == start:
                break
        return path

    def emit(path):
        closed = len(path) > 2 and path[-1] == path[0]
        if closed:
            path = path[:-1]
        return [cell_centre(*c) for c in path], closed

    raw = []
    # Ends and junctions first, so whatever edges are left over are pure loops.
    for node in sorted(cells):
        if len(adj[node]) == 2:
            continue
        if not adj[node]:
            raw.append((cell_box(node), True))
            continue
        for n in sorted(adj[node]):
            if frozenset((node, n)) not in used:
                raw.append(emit(walk(node, n)))
    for node in sorted(cells):
        for n in sorted(adj[node]):
            if frozenset((node, n)) not in used:
                raw.append(emit(walk(node, n)))

    out = []
    for pts, loop in raw:
        pts = simplify_polyline(pts, loop)
        if len(pts) >= 2:
            out.append((pts, loop))
    return out


def is_on_grid(points: List[Tuple[float, float]]) -> bool:
    """True when every corner already sits on a grid line."""
    return all(abs(v - round(v / CELL_PX) * CELL_PX) < 1e-6 for p in points for v in p)


def offset_to_cell_edges(points: List[Tuple[float, float]], loop: bool,
                        inside: Set[Tuple[int, int]]) -> List[Tuple[float, float]]:
    """Move a wall line off the middle of its squares and onto the grid lines.

    A wall traced through the centres of its tiles cuts every bordering square
    in half, and half a square is not a square a token can stand on. Each run is
    pushed the half cell that puts it on the **outer** edge of its own tiles -
    the footprint of the building, the outside of a hull - so the ground the
    wall encloses keeps whole squares all the way to the wall.

    Which way is out follows from the winding for a closed ring, so it cannot
    disagree with itself halfway round; an open run asks the map which side is
    enclosed ground and goes the other way.
    """
    n = len(points)
    segs = n if loop else n - 1
    # A lone wall tile is already drawn as a box on the grid lines; offsetting
    # it inward by half a cell from all four sides would collapse it to a point.
    if segs < 1 or is_on_grid(points):
        return list(points)

    # A closed ring is offset towards its own interior; which side that is
    # follows from the winding, so it needs no lookups and never disagrees
    # with itself halfway round.
    outward = None
    if loop:
        area = sum(points[i][0] * points[(i + 1) % n][1] - points[(i + 1) % n][0] * points[i][1]
                   for i in range(n))
        # (uy, -ux) points into a ring wound this way, so the sign that takes
        # the line out of it is the other one.
        outward = -1.0 if area < 0 else 1.0

    lines = []           # (point on the offset line, direction)
    for i in range(segs):
        ax, ay = points[i]
        bx, by = points[(i + 1) % n]
        dx, dy = bx - ax, by - ay
        length = math.hypot(dx, dy)
        if length < 1e-6:
            lines.append(((ax, ay), (1.0, 0.0)))
            continue
        ux, uy = dx / length, dy / length
        normal = (uy * outward, -ux * outward) if outward is not None else None

        if normal is None:
            # An open run has no interior, so ask the map which side is ground
            # and take the other one. The probe reaches a whole cell out, not
            # half: half a cell lands on the boundary of the wall tile itself,
            # which is neither side and made the vote a coin toss - that is how
            # a hull's bow came back folded through itself.
            best = None
            for cand in ((uy, -ux), (-uy, ux)):
                score = 0
                for step in range(1, 6):
                    t = step / 6.0
                    mx = ax + dx * t + cand[0] * CELL_PX
                    my = ay + dy * t + cand[1] * CELL_PX
                    if (int(mx // CELL_PX), int(my // CELL_PX)) in inside:
                        score += 1
                # Ties fall to +x / +y so a free-standing run still lands on a
                # grid line instead of staying stranded in mid-square.
                canonical = 1 if (cand[0] > 0.5 or cand[1] > 0.5) else 0
                if best is None or (-score, canonical) > (-best[0], best[1]):
                    best = (score, canonical, cand)
            normal = best[2]

        half = CELL_PX / 2.0
        lines.append(((ax + normal[0] * half, ay + normal[1] * half), (ux, uy)))

    def meet(i, j):
        """Where two offset lines cross, or the shared end if they are parallel."""
        (px, py), (ux, uy) = lines[i]
        (qx, qy), (vx, vy) = lines[j]
        denom = ux * vy - uy * vx
        if abs(denom) < 1e-9:
            return (qx, qy)
        t = ((qx - px) * vy - (qy - py) * vx) / denom
        return (px + ux * t, py + uy * t)

    out = []
    if loop:
        for i in range(segs):
            out.append(meet(i - 1, i))
    else:
        (px, py), _ = lines[0]
        out.append((px, py))
        for i in range(1, segs):
            out.append(meet(i - 1, i))
        (qx, qy), (vx, vy) = lines[segs - 1]
        ex, ey = points[segs]
        # Project the original end onto its offset line, so the run keeps its
        # length instead of being cut short by the offset.
        t = (ex - qx) * vx + (ey - qy) * vy
        out.append((qx + vx * t, qy + vy * t))

    # A segment shorter than the offset can come back pointing the other way,
    # which draws the wall folded through itself. Drop those rather than ship a
    # knot; the neighbours meet directly instead.
    for _pass in range(3):
        doubled = [i for i in range(segs)
                   if (out[(i + 1) % len(out)][0] - out[i][0]) * (points[(i + 1) % n][0] - points[i][0])
                   + (out[(i + 1) % len(out)][1] - out[i][1]) * (points[(i + 1) % n][1] - points[i][1]) < 0]
        if not doubled or segs - len(doubled) < 2:
            break
        keep = [i for i in range(segs) if i not in set(doubled)]
        lines = [lines[i] for i in keep]
        points = [points[i] for i in keep] + ([] if loop else [points[segs]])
        n = len(points)
        segs = len(lines)
        out = ([meet(i - 1, i) for i in range(segs)] if loop
               else [lines[0][0]] + [meet(i - 1, i) for i in range(1, segs)] + [out[-1]])

    return [(round(x, 3), round(y, 3)) for x, y in out]


def trace_region_rings(cells: Set[Tuple[int, int]]) -> List[Tuple[List[Tuple[float, float]], bool]]:
    """Outline a region of cells as closed rings running along the cell edges.

    Returns (points_in_pixels, is_hole) pairs. Outer rings wind one way, rings
    around an enclosed gap wind the other.
    """
    out_edges: Dict[Tuple[int, int], List[Tuple[int, int]]] = {}

    def add(a, b):
        out_edges.setdefault(a, []).append(b)

    for (x, y) in cells:
        if (x, y - 1) not in cells:
            add((x, y), (x + 1, y))
        if (x + 1, y) not in cells:
            add((x + 1, y), (x + 1, y + 1))
        if (x, y + 1) not in cells:
            add((x + 1, y + 1), (x, y + 1))
        if (x - 1, y) not in cells:
            add((x, y + 1), (x, y))

    rings = []
    while out_edges:
        start = next(iter(out_edges))
        ring = [start]
        cur = start
        heading = None
        while True:
            options = out_edges.get(cur)
            if not options:
                break
            step = None
            if heading is not None:
                for cand in options:
                    if (cand[0] - cur[0], cand[1] - cur[1]) == heading:
                        step = cand
                        break
            if step is None:
                step = options[0]
            options.remove(step)
            if not options:
                del out_edges[cur]
            heading = (step[0] - cur[0], step[1] - cur[1])
            cur = step
            if cur == start:
                break
            ring.append(cur)
        if len(ring) < 4:
            continue
        pts = [(px * CELL_PX, py * CELL_PX) for px, py in ring]
        area = 0.0
        for i in range(len(pts)):
            x1, y1 = pts[i]
            x2, y2 = pts[(i + 1) % len(pts)]
            area += x1 * y2 - x2 * y1
        rings.append((simplify_polyline(pts, True), area < 0))
    return rings


def point_in_ring(point: Tuple[float, float], ring: List[Tuple[float, float]]) -> bool:
    """Even-odd containment test, used to hang holes off the right shoreline."""
    x, y = point
    inside = False
    n = len(ring)
    for i in range(n):
        x1, y1 = ring[i]
        x2, y2 = ring[(i + 1) % n]
        if (y1 > y) != (y2 > y):
            crossing = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
            if x < crossing:
                inside = not inside
    return inside


def ring_centroid(ring: List[Tuple[float, float]]) -> Tuple[float, float]:
    """Average of a ring's corners - inside it for every shape we trace."""
    return (sum(p[0] for p in ring) / len(ring), sum(p[1] for p in ring) / len(ring))


def water_node(points: List[Tuple[float, float]], children: List[dict]) -> dict:
    """One body of water in the level's water tree."""
    return {
        "ref": random.randint(10000000, 999999999),
        "polygon": format_pool_vector2_array(points),
        "join": 0,
        "end": 0,
        "is_open": False,
        "deep_color": WATER_DEEP_COLOR,
        "shallow_color": WATER_SHALLOW_COLOR,
        "blend_distance": WATER_BLEND_DISTANCE,
        "children": children,
    }


def generate_cave_bitmaps(
    cols: int,
    rows: int,
    floor_cells: Set[Tuple[int, int]],
    is_cave: bool,
) -> Tuple[str, str]:
    """Generate Dungeondraft cave and entrance bitmasks.

    1 bit per sample, packed 8 to a byte, least significant bit first,
    over a grid of (4w + 3) x (4h + 3) samples. The three extra samples per
    axis straddle the map edge, 1.5 of them on each side.
    """
    cw = 4 * cols + 3
    ch = 4 * rows + 3
    total_bits = cw * ch
    total_bytes = (total_bits + 7) // 8
    cave_buf = bytearray(total_bytes)
    entrance_buf = bytearray(total_bytes)

    if not is_cave or not floor_cells:
        return format_pool_byte_array(bytes(cave_buf)), format_pool_byte_array(bytes(entrance_buf))

    for sy in range(ch):
        gy = int(math.floor((sy - 1.5) / 4.0))
        row_base = sy * cw
        for sx in range(cw):
            if (int(math.floor((sx - 1.5) / 4.0)), gy) not in floor_cells:
                continue
            bit_index = row_base + sx
            cave_buf[bit_index // 8] |= (1 << (bit_index % 8))

    return format_pool_byte_array(bytes(cave_buf)), format_pool_byte_array(bytes(entrance_buf))


class DungeondraftAssembler:
    """Constructs valid .dungeondraft_map data from a map plan."""

    def __init__(self, matcher: Optional[DungeondraftMatcher] = None, db_path: Optional[Path] = None):
        self.matcher = matcher or DungeondraftMatcher(db_path=db_path)
        self.node_counter = 0

    def _next_node_id(self) -> str:
        """Generate the next unique node_id as a lowercase hex string."""
        nid = hex(self.node_counter)[2:]
        self.node_counter += 1
        return nid

    def assemble(self, map_plan: dict, seed: int = 42) -> Tuple[dict, dict]:
        """Assemble map_plan JSON into (dungeondraft_map_dict, sidecar_report_dict)."""
        self.node_counter = 0
        random.seed(seed)

        grid = map_plan.get("grid", {})
        cols = int(grid.get("cols", 25))
        rows = int(grid.get("rows", 25))
        meta = map_plan.get("meta", {})
        title = meta.get("title", "Generated Battlemap")
        style_id = meta.get("style", "default")
        enclosure = meta.get("enclosure", "masonry")

        areas = map_plan.get("areas", [])
        zones = map_plan.get("zones", [])
        features = map_plan.get("features", [])
        annotations = map_plan.get("annotations", [])
        effects = map_plan.get("effects", [])

        # The empty ring around the field exists so a diffusion model makes its
        # mistakes in blank space. Dungeondraft has no such problem, and an
        # editable map that opens two squares of nothing wider than it should be
        # is just wrong, so the margin is cut off here.
        border = max(0, int(meta.get("border", 0) or 0))
        if border:
            cols = max(1, cols - 2 * border)
            rows = max(1, rows - 2 * border)
            areas, zones, features, annotations, effects = (
                shift_items(items, -border) for items in
                (areas, zones, features, annotations, effects))

        packs_referenced: Set[str] = set()

        is_cave = (
            enclosure == "rock"
            or "cave" in style_id.lower()
            or "cavern" in style_id.lower()
            or meta.get("layout") in ("cavern", "cave")
        )

        # Everything geometric below is built from the rasterized plan rather
        # than from the rect zones directly. Those rectangles are a compressed
        # decomposition of a tile grid: rebuilding walls or shorelines out of
        # them is what leaves gaps at corners and seams down the middle of a
        # lake.
        zones_grid = zones_to_grid({"grid": {"cols": cols, "rows": rows},
                                    "zones": zones})

        # 1. Floor Tiles
        # cells length = cols * rows in row-major order (-1 is empty/background)
        tiles_cells = [-1] * (cols * rows)
        floor_tileset_res, tileset_pack = self.matcher.match_floor_tileset(style_id=style_id)
        if tileset_pack and tileset_pack != "default":
            packs_referenced.add(tileset_pack)

        tiles_lookup = dict(DEFAULT_TILES_LOOKUP)
        deck_index = None
        if floor_tileset_res and not is_cave:
            tiles_lookup["0"] = floor_tileset_res

            # Decking is timber even when the map is stone, so a gangway or a
            # ship's deck gets its own slot rather than the quay's flagstones.
            deck_res, deck_pack = self.matcher.match_floor_tileset(style_id="wood plank")
            if deck_res and deck_res != floor_tileset_res:
                # Slot 1, not a new one past the end: Dungeondraft ships 24
                # tilesets at indices 0-23 and a cell pointing past them draws
                # nothing, which is how a ship's deck came out as bare ground.
                deck_index = 1
                tiles_lookup[str(deck_index)] = deck_res
                if deck_pack and deck_pack != "default":
                    packs_referenced.add(deck_pack)

            paved = {FLOOR, DOOR, WINDOW, STAIRS}
            deck_idx = deck_index if deck_index is not None else 0
            for y in range(rows):
                for x in range(cols):
                    kind = zones_grid.get(x, y)
                    idx = None
                    if kind in paved:
                        idx = 0
                    elif kind == BRIDGE:
                        idx = deck_idx
                    elif kind == WALL:
                        # Floor runs underneath a wall. Leaving the wall band
                        # bare is what makes a finished room read as a sketch.
                        neighbours = [zones_grid.get(x + dx, y + dy)
                                      for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))]
                        if any(k in paved for k in neighbours):
                            idx = 0
                        elif BRIDGE in neighbours:
                            idx = deck_idx
                    if idx is not None:
                        tiles_cells[y * cols + x] = idx

            if all(c < 0 for c in tiles_cells):
                # A plan with areas but no tile zones - fall back to the rooms.
                for area in areas:
                    ax, ay = int(area.get("x", 0)), int(area.get("y", 0))
                    aw, ah = int(area.get("w", 0)), int(area.get("h", 0))
                    for y in range(max(0, ay), min(rows, ay + ah)):
                        for x in range(max(0, ax), min(cols, ax + aw)):
                            tiles_cells[y * cols + x] = 0

        # One colour per cell, not per tileset: Dungeondraft indexes this array
        # by cell, and an empty one leaves every tile untinted to black.
        tiles_colors = ["ffffffff"] * (cols * rows)

        # 2. Terrain Splatmap (4 samples/cell/axis)
        splat_w = cols * 4
        splat_h = rows * 4
        splat_sample_count = splat_w * splat_h
        splat1_bytes = bytearray([255, 0, 0, 0] * splat_sample_count)

        terrain_slots = self.matcher.match_terrain_textures(style_id=style_id)

        # Paint the ground by what the plan says is there, but only into slots
        # the library actually filled - a weight on an empty texture slot is a
        # hole in the map.
        painted = {kind: ch for kind, ch in TERRAIN_CHANNEL_BY_KIND.items()
                   if len(terrain_slots) > ch and terrain_slots[ch][0]}
        if painted:
            for sy in range(splat_h):
                row = sy * splat_w
                for sx in range(splat_w):
                    channel = painted.get(zones_grid.get(sx // 4, sy // 4))
                    if channel is None:
                        continue
                    base = (row + sx) * 4
                    splat1_bytes[base] = 0
                    splat1_bytes[base + channel] = 255

        terrain_dict = {
            "enabled": True,
            "expand_slots": False,
            "smooth_blending": True,
            "texture_1": terrain_slots[0][0] if len(terrain_slots) > 0 else "res://textures/terrain/terrain_dirt.png",
            "texture_2": terrain_slots[1][0] if len(terrain_slots) > 1 else "res://textures/terrain/terrain_gravel.png",
            "texture_3": terrain_slots[2][0] if len(terrain_slots) > 2 else "res://textures/terrain/terrain_sand.png",
            "texture_4": terrain_slots[3][0] if len(terrain_slots) > 3 else "res://textures/terrain/terrain_snow.png",
            "splat": format_pool_byte_array(bytes(splat1_bytes)),
        }
        for t_res, t_pack in terrain_slots[:4]:
            if t_pack and t_pack != "default":
                packs_referenced.add(t_pack)

        # 3. Walls & Portals
        wall_texture_res, wall_pack = self.matcher.match_wall_texture(style_id=style_id, enclosure=enclosure)
        if wall_pack and wall_pack != "default":
            packs_referenced.add(wall_pack)

        door_texture_res, door_pack = self.matcher.match_portal_texture("door")
        if door_pack and door_pack != "default":
            packs_referenced.add(door_pack)

        walls_list = []
        wall_points: List[List[Tuple[float, float]]] = []   # parallel to walls_list
        wall_loops: List[bool] = []
        shapes_polygons = []
        shapes_walls = []

        window_texture_res, window_pack = self.matcher.match_portal_texture("window")
        if window_pack and window_pack != "default":
            packs_referenced.add(window_pack)

        wall_cells = cells_of_kind(zones_grid, {WALL})
        opening_cells = cells_of_kind(zones_grid, {DOOR, WINDOW})
        # A doorway is a gap the plan punched through a wall run. Feeding those
        # cells back into the wall network keeps the wall continuous across the
        # opening, which is what a portal needs: Dungeondraft cuts the doorway
        # into the wall rather than butting two walls up against a hole.
        wired_openings = {
            c for c in opening_cells
            if any((c[0] + dx, c[1] + dy) in wall_cells
                   for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
        }

        if wall_cells:
            # A run of wall tiles becomes a centreline; a solid mass becomes its
            # outline, holes in it included.
            enclosed = cells_of_kind(zones_grid, ENCLOSED_KINDS)
            wall_shapes: List[Tuple[List[Tuple[float, float]], bool]] = []
            for region in connected_cell_groups(wall_cells | wired_openings, diagonal=True):
                if is_thin_region(region):
                    wall_shapes.extend(
                        (offset_to_cell_edges(pts, loop, enclosed), loop)
                        for pts, loop in trace_wall_polylines(region))
                else:
                    # A solid mass is already outlined along the cell edges.
                    wall_shapes.extend((ring, True) for ring, _hole in trace_region_rings(region))

            for pts, loop in wall_shapes:
                walls_list.append({
                    "points": format_pool_vector2_array(pts),
                    "texture": wall_texture_res,
                    "color": "ffffffff",
                    "loop": loop,
                    "type": 1,
                    "joint": 1,
                    "normalize_uv": True,
                    "shadow": True,
                    "node_id": self._next_node_id(),
                    "portals": [],
                })
                wall_points.append(pts)
                wall_loops.append(loop)
        elif not is_cave:
            # Fallback for plans that carry rooms but no wall tiles at all.
            for area in areas:
                label = area.get("label", "").lower()
                if any(w in label for w in ("quay", "ship", "boat", "water", "river", "lake", "bridge", "open", "yard")):
                    continue
                ax, ay = int(area.get("x", 0)), int(area.get("y", 0))
                aw, ah = int(area.get("w", 0)), int(area.get("h", 0))
                if aw <= 0 or ah <= 0:
                    continue

                poly_pts = [(ax * CELL_PX, ay * CELL_PX),
                            ((ax + aw) * CELL_PX, ay * CELL_PX),
                            ((ax + aw) * CELL_PX, (ay + ah) * CELL_PX),
                            (ax * CELL_PX, (ay + ah) * CELL_PX)]

                walls_list.append({
                    "points": format_pool_vector2_array(poly_pts),
                    "texture": wall_texture_res,
                    "color": "ffffffff",
                    "loop": True,
                    "type": 1,
                    "joint": 1,
                    "normalize_uv": True,
                    "shadow": True,
                    "node_id": self._next_node_id(),
                    "portals": [],
                })
                wall_points.append(poly_pts)
                wall_loops.append(True)

        # Cave bitmasks
        cave_floor_cells = cells_of_kind(zones_grid, CAVE_FLOOR_KINDS)
        if not cave_floor_cells:
            for area in areas:
                ax, ay = int(area.get("x", 0)), int(area.get("y", 0))
                aw, ah = int(area.get("w", 0)), int(area.get("h", 0))
                for y in range(max(0, ay), min(rows, ay + ah)):
                    for x in range(max(0, ax), min(cols, ax + aw)):
                        cave_floor_cells.add((x, y))
        cave_bitmap_str, cave_entrance_str = generate_cave_bitmaps(
            cols, rows, cave_floor_cells, is_cave)

        # Doorways and windows become portals cut into the wall they interrupt.
        # A run of adjacent opening cells is one portal, as wide as the run.
        portal_cells = wired_openings if wall_cells else opening_cells
        openings = []
        for kind, texture in ((DOOR, door_texture_res), (WINDOW, window_texture_res)):
            of_kind = {c for c in portal_cells if zones_grid.get(c[0], c[1]) == kind}
            for group in connected_cell_groups(of_kind):
                xs = [c[0] for c in group]
                ys = [c[1] for c in group]
                span = max(max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)
                openings.append({
                    "cx": (sum(xs) / len(xs) + 0.5) * CELL_PX,
                    "cy": (sum(ys) / len(ys) + 0.5) * CELL_PX,
                    "radius": int(round(span * CELL_PX / 2.0)),
                    "texture": texture,
                    "id": f"{kind}@{min(xs)},{min(ys)}",
                })

        unattached_doors = []
        for opening in sorted(openings, key=lambda o: (o["cy"], o["cx"])):
            best = None
            best_wall = -1
            for wi, pts in enumerate(wall_points):
                hit = nearest_point_on_polyline(pts, wall_loops[wi], opening["cx"], opening["cy"])
                if hit and (best is None or hit[0] < best[0]):
                    best, best_wall = hit, wi

            if best is None or best[0] > DOOR_SNAP_PX:
                unattached_doors.append({"id": opening["id"],
                                         "x": opening["cx"] / CELL_PX,
                                         "y": opening["cy"] / CELL_PX})
                continue

            _dist, seg_index, t, px, py, rotation, (ux, uy) = best
            walls_list[best_wall]["portals"].append({
                "position": format_vector2(px, py),
                "rotation": round(rotation, 6),
                "scale": "Vector2( 1, 1 )",
                "direction": format_vector2(ux, uy),
                "texture": opening["texture"],
                "radius": opening["radius"],
                "point_index": seg_index,
                # The wall this portal is cut into, by its node_id. Writing "0"
                # for every portal binds them all to whichever wall is node 0,
                # and a portal bound to the wrong wall neither opens the wall it
                # sits on nor leaves it alone - the wall renders as a diagonal
                # through the room.
                "wall_id": walls_list[best_wall]["node_id"],
                # Measured along the whole polyline, not within one segment:
                # the whole number is the segment and the fraction is the
                # position along it. A real map has a portal at 3.5.
                "wall_distance": round(seg_index + t, 6),
                "closed": True,
                "node_id": self._next_node_id(),
            })

        # Water bodies. One polygon per connected body, traced along the cell
        # edges: emitting the plan's rectangles one at a time instead puts a
        # seam down every join and a straight cut where a shoreline should turn.
        water_cells = cells_of_kind(zones_grid, {WATER})
        if not water_cells:
            for wz in (z for z in zones if z.get("kind") == WATER):
                wx, wy = int(wz.get("x", 0)), int(wz.get("y", 0))
                ww, wh = int(wz.get("w", 1)), int(wz.get("h", 1))
                for y in range(max(0, wy), min(rows, wy + wh)):
                    for x in range(max(0, wx), min(cols, wx + ww)):
                        water_cells.add((x, y))

        water_children = []
        for body in connected_cell_groups(water_cells):
            rings = trace_region_rings(body)
            outers = [r for r, is_hole in rings if not is_hole]
            holes = [r for r, is_hole in rings if is_hole]
            for outer in outers:
                enclosed = [h for h in holes if point_in_ring(ring_centroid(h), outer)]
                water_children.append(
                    water_node(outer, [water_node(h, []) for h in enclosed]))

        # The root of the tree holds no water of its own - a real Dungeondraft
        # file leaves its colours fully transparent and its blend at zero.
        water_dict = {
            "disable_border": False,
            "tree": {
                "ref": -496340410,
                "polygon": "PoolVector2Array(  )",
                "join": 0,
                "end": 0,
                "is_open": False,
                "deep_color": "00000000",
                "shallow_color": "00000000",
                "blend_distance": 0,
                "children": water_children,
            },
        }

        # 4. Placed Objects (Props)
        objects_list = []
        lights_list = []
        matched_props_report = []
        unmatched_props_report = []

        light_tex_res, light_pack = DEFAULT_STOCK_LIGHT, "default"

        for feat in features:
            kind = feat.get("kind", "prop")
            fx = float(feat.get("x", 0))
            fy = float(feat.get("y", 0))
            fw = max(float(feat.get("w", 1) or 1), 0.25)
            fh = max(float(feat.get("h", 1) or 1), 0.25)
            rot_deg = float(feat.get("rotation", random.choice([0, 90, 180, 270])))
            rot_rad = math.radians(rot_deg)

            center_px_x = (fx + fw / 2.0) * 256.0
            center_px_y = (fy + fh / 2.0) * 256.0

            prop_match = self.matcher.match_prop(
                prop_kind=kind,
                style_id=style_id,
                seed=seed + int(fx * 31 + fy),
            )

            if prop_match:
                p_res = prop_match["res_path"]
                p_pack = prop_match.get("pack_id", "default")
                if p_pack and p_pack != "default":
                    packs_referenced.add(p_pack)

                # Scale only when the art is far bigger than the space the plan
                # gave it: 256 authored pixels are one grid square, so a 14-cell
                # statue dropped on a one-cell feature swallows the room. Small
                # art is left alone - half the object library is under half a
                # cell on purpose, and inflating a coin to a full square is the
                # same mistake in the other direction.
                nat_w = float(prop_match.get("grid_w") or 0.0)
                nat_h = float(prop_match.get("grid_h") or 0.0)
                fit = 1.0
                if nat_w > 0 and nat_h > 0:
                    over = max(nat_w / fw, nat_h / fh)
                    if over > PROP_OVERSIZE_TOLERANCE:
                        fit = round(1.0 / over, 4)

                obj_nid = self._next_node_id()
                objects_list.append({
                    "position": format_vector2(center_px_x, center_px_y),
                    "rotation": round(rot_rad, 5),
                    "scale": format_vector2(fit, fit),
                    "mirror": False,
                    "texture": p_res,
                    "layer": 100,
                    "shadow": True,
                    "block_light": False,
                    "node_id": obj_nid,
                })
                matched_props_report.append({
                    "kind": kind,
                    "matched_asset": p_res,
                    "pack_id": p_pack,
                    "x": fx, "y": fy,
                    "scale": fit,
                    # "described" means the catalogue answered; "named" means we
                    # only matched the file name, which is a weaker claim.
                    "match_quality": prop_match.get("match_quality", "named"),
                })

                # If this prop is a light source (hearth, brazier, torch, lamp, fire), place a light
                if any(w in kind.lower() for w in ("hearth", "brazier", "torch", "lamp", "fire", "candle", "lantern")):
                    light_nid = self._next_node_id()
                    lights_list.append({
                        "position": format_vector2(center_px_x, center_px_y),
                        "rotation": 0.0,
                        "range": 5.0,
                        "intensity": 1.0,
                        "color": "ffeccd8b",
                        "texture": light_tex_res,
                        "shadows": True,
                        "node_id": light_nid,
                    })
            else:
                unmatched_props_report.append({"kind": kind, "x": fx, "y": fy})

        # 5. Build Asset Manifest
        # Query packs table for manifest details of referenced packs
        asset_manifest = []
        if packs_referenced:
            cur = self.matcher.db.conn.cursor()
            placeholders = ",".join("?" for _ in packs_referenced)
            cur.execute(f"SELECT id, name, author, version FROM packs WHERE id IN ({placeholders});", list(packs_referenced))
            for row in cur.fetchall():
                asset_manifest.append({
                    "name": row["name"],
                    "id": row["id"],
                    "version": str(row["version"] or "1.0"),
                    "author": row["author"] or "Unknown",
                    "keywords": None,
                    "allow_3rd_party_mapping_software_to_read": True,
                    "custom_color_overrides": {},
                })

        now = datetime.datetime.now()
        creation_date = {
            "year": now.year,
            "month": now.month,
            "day": now.day,
            # Godot counts weekdays from Sunday; Python counts from Monday.
            "weekday": (now.weekday() + 1) % 7,
            "dst": False,
            "hour": now.hour,
            "minute": now.minute,
            "second": now.second,
        }

        # 6. Assemble Full Map JSON
        dungeondraft_map = {
            "header": {
                "creation_build": "1.2.0.1 opulent kirin",
                "creation_date": creation_date,
                "uses_default_assets": True,
                "asset_manifest": asset_manifest,
                "editor_state": {
                    "current_level": 0,
                    "camera_position": f"Vector2( {cols * 128}, {rows * 128} )",
                    "camera_zoom": 1,
                    "guide_position": "null",
                    "trace_image": None,
                    "color_palettes": DEFAULT_COLOR_PALETTES,
                    "object_tags_memory": {"set": 0, "tags": []},
                    "scatter_tags_memory": {"set": 0, "tags": []},
                    "object_library_memory": {
                        "mode": "all",
                        "scroll": 0,
                        "selected": [],
                        "search_strictness": 0.5,
                    },
                    "scatter_library_memory": None,
                    "path_library_memory": None,
                    "sharpen_fonts": True,
                },
            },
            "world": {
                "format": 3,
                "width": cols,
                "height": rows,
                "next_node_id": hex(self.node_counter + 1)[2:],
                "next_prefab_id": "0",
                "msi": {
                    "offset_map_size": 512,
                    "max_offset_distance": 0.2,
                    "cell_size": 64,
                    "seed": f"{seed & 0x7FFFFFFF:x}",
                },
                "grid": {
                    "color": "7f000000",
                    "texture": "res://textures/grid/dotted_line.png",
                },
                "wall_shadow": True,
                "object_shadow": True,
                "building_wear": None,
                "trace_image_visible": False,
                "embedded": {},
                "levels": {
                    "0": {
                        "label": "Ground",
                        "layers": {
                            "-400": "Below Ground",
                            "-100": "Below Water",
                            "100": "User Layer 1",
                            "200": "User Layer 2",
                            "300": "User Layer 3",
                            "400": "User Layer 4",
                            "700": "Above Walls",
                            "900": "Above Roofs",
                        },
                        "environment": {
                            "baked_lighting": True,
                            "ambient_light": "ffffffff",
                        },
                        "tiles": {
                            "cells": format_pool_int_array(tiles_cells),
                            "colors": tiles_colors,
                            "lookup": tiles_lookup,
                        },
                        "terrain": terrain_dict,
                        "cave": {
                            "bitmap": cave_bitmap_str,
                            "entrance_bitmap": cave_entrance_str,
                            "ground_color": "ff7f7e71",
                            "wall_color": "ff7f7e71",
                            "texture": "res://textures/caves/colorable/floor.png",
                        },
                        "water": water_dict,
                        "shapes": {
                            "polygons": shapes_polygons,
                            "walls": shapes_walls,
                        },
                        "materials": {
                            "-400": [],
                        },
                        "patterns": [],
                        "paths": [],
                        "objects": objects_list,
                        "walls": walls_list,
                        "portals": [],
                        "lights": lights_list,
                        "texts": [],
                        "texts_vis": True,
                        "roofs": {
                            "shade": True,
                            "shade_contrast": 0.5,
                            "sun_direction": 45,
                            "roofs": [],
                        },
                    }
                },
            },
            "mod": {
                ".node_table": {},
            },
        }

        report = {
            "title": title,
            "style": style_id,
            "grid": {"cols": cols, "rows": rows},
            "packs_referenced_count": len(packs_referenced),
            "packs_referenced": list(packs_referenced),
            "props_matched_by_description": sum(1 for m in matched_props_report
                                                if m["match_quality"] == "described"),
            "props_matched_by_name": sum(1 for m in matched_props_report
                                         if m["match_quality"] == "named"),
            "walls_placed": len(walls_list),
            "portals_placed": sum(len(w.get("portals", [])) for w in walls_list),
            "objects_placed": len(objects_list),
            "lights_placed": len(lights_list),
            "doors_unattached": len(unattached_doors),
            "matched_props": matched_props_report,
            "unmatched_props": unmatched_props_report,
            "unattached_doors": unattached_doors,
        }

        return dungeondraft_map, report

    def save_map(self, map_plan: dict, out_path: str, seed: int = 42) -> Tuple[str, str]:
        """Generate and save .dungeondraft_map and its sidecar .report.json."""
        out_file = Path(out_path)
        out_file.parent.mkdir(parents=True, exist_ok=True)

        dd_map, report = self.assemble(map_plan, seed=seed)

        # Write .dungeondraft_map JSON
        with open(out_file, "w", encoding="utf-8") as f:
            json.dump(dd_map, f, indent=2)

        # Write sidecar .report.json
        report_file = out_file.with_suffix(".report.json")
        with open(report_file, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)

        return str(out_file), str(report_file)
