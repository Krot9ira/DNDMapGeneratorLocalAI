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
DOOR_SNAP_PX = 384          # 1.5 cells: a door zone and the wall it belongs to
                            # are drawn on different grids and land up to half a
                            # cell apart, so an exact test never matches.

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


def is_segment_redundant_with_rooms(
    pts: List[Tuple[float, float]],
    room_polys: List[List[Tuple[float, float]]],
    tol: float = 256.0,
) -> bool:
    """Check if a wall line segment runs collinear and parallel within tol of an existing room edge."""
    if len(pts) < 2 or not room_polys:
        return False
    p1, p2 = pts[0], pts[1]
    dx1, dy1 = p2[0] - p1[0], p2[1] - p1[1]
    len1 = math.hypot(dx1, dy1)
    if len1 < 1.0:
        return False
    is_h1 = abs(dy1) <= 1.0
    is_v1 = abs(dx1) <= 1.0

    for poly in room_polys:
        n = len(poly)
        for i in range(n):
            e1 = poly[i]
            e2 = poly[(i + 1) % n]
            dx2, dy2 = e2[0] - e1[0], e2[1] - e1[1]
            len2 = math.hypot(dx2, dy2)
            if len2 < 1.0:
                continue
            is_h2 = abs(dy2) <= 1.0
            is_v2 = abs(dx2) <= 1.0
            if is_h1 and is_h2:
                if abs(p1[1] - e1[1]) <= tol:
                    min_x1, max_x1 = min(p1[0], p2[0]), max(p1[0], p2[0])
                    min_x2, max_x2 = min(e1[0], e2[0]), max(e1[0], e2[0])
                    overlap = min(max_x1, max_x2) - max(min_x1, min_x2)
                    if overlap >= min(len1, len2) * 0.7:
                        return True
            elif is_v1 and is_v2:
                if abs(p1[0] - e1[0]) <= tol:
                    min_y1, max_y1 = min(p1[1], p2[1]), max(p1[1], p2[1])
                    min_y2, max_y2 = min(e1[1], e2[1]), max(e1[1], e2[1])
                    overlap = min(max_y1, max_y2) - max(min_y1, min_y2)
                    if overlap >= min(len1, len2) * 0.7:
                        return True
    return False


def generate_cave_bitmaps(
    cols: int,
    rows: int,
    areas: List[dict],
    zones: List[dict],
    is_cave: bool,
) -> Tuple[str, str]:
    """Generate Dungeondraft cave and entrance bitmasks.

    1 bit per sample, packed 8 to a byte, least significant bit first,
    over a grid of (4w + 3) x (4h + 3) samples.
    """
    cw = 4 * cols + 3
    ch = 4 * rows + 3
    total_bits = cw * ch
    total_bytes = (total_bits + 7) // 8
    cave_buf = bytearray(total_bytes)
    entrance_buf = bytearray(total_bytes)

    if not is_cave:
        return format_pool_byte_array(bytes(cave_buf)), format_pool_byte_array(bytes(entrance_buf))

    # Floor regions in cell units
    floor_rects = []
    for a in areas:
        floor_rects.append((float(a.get("x", 0)), float(a.get("y", 0)), float(a.get("w", 0)), float(a.get("h", 0))))
    for z in zones:
        if z.get("kind") in ("floor", "passage", "corridor", "water", "room", "cavern"):
            floor_rects.append((float(z.get("x", 0)), float(z.get("y", 0)), float(z.get("w", 0)), float(z.get("h", 0))))

    for sy in range(ch):
        gy = sy / 4.0
        for sx in range(cw):
            gx = sx / 4.0
            bit_index = sy * cw + sx
            byte_idx = bit_index // 8
            bit_offset = bit_index % 8

            is_floor = False
            for rx, ry, rw, rh in floor_rects:
                if rx <= gx <= rx + rw and ry <= gy <= ry + rh:
                    is_floor = True
                    break

            if is_floor:
                cave_buf[byte_idx] |= (1 << bit_offset)

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

        packs_referenced: Set[str] = set()

        is_cave = (
            enclosure == "rock"
            or "cave" in style_id.lower()
            or "cavern" in style_id.lower()
            or meta.get("layout") in ("cavern", "cave")
        )

        # 1. Floor Tiles
        # cells length = cols * rows in row-major order (-1 is empty/background)
        tiles_cells = [-1] * (cols * rows)
        floor_tileset_res, tileset_pack = self.matcher.match_floor_tileset(style_id=style_id)
        if tileset_pack and tileset_pack != "default":
            packs_referenced.add(tileset_pack)

        tiles_lookup = dict(DEFAULT_TILES_LOOKUP)
        if floor_tileset_res and not is_cave:
            tiles_lookup["0"] = floor_tileset_res
            # Fill area cells with tile index 0
            for area in areas:
                ax, ay = int(area.get("x", 0)), int(area.get("y", 0))
                aw, ah = int(area.get("w", 0)), int(area.get("h", 0))
                for y in range(ay, min(rows, ay + ah)):
                    for x in range(ax, min(cols, ax + aw)):
                        tiles_cells[y * cols + x] = 0

        tiles_colors = ["ffffffff"] * (cols * rows)

        # 2. Terrain Splatmap (4 samples/cell/axis)
        splat_w = cols * 4
        splat_h = rows * 4
        splat_sample_count = splat_w * splat_h
        splat1_bytes = bytearray([255, 0, 0, 0] * splat_sample_count)

        terrain_slots = self.matcher.match_terrain_textures(style_id=style_id)
        terrain_dict = {
            "enabled": True,
            "expand_slots": False,
            "smooth_blending": False,
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
        room_polygons_pts: List[List[Tuple[float, float]]] = []

        door_zones = [z for z in zones if z.get("kind") == "door"]

        # Room outlines (only for built/structural rooms, not natural caves)
        if not is_cave:
            for area in areas:
                ax, ay = int(area.get("x", 0)), int(area.get("y", 0))
                aw, ah = int(area.get("w", 0)), int(area.get("h", 0))
                if aw <= 0 or ah <= 0:
                    continue

                # Convert to 256px coordinates
                px_x1 = ax * 256
                px_y1 = ay * 256
                px_x2 = (ax + aw) * 256
                px_y2 = (ay + ah) * 256

                poly_pts = [(px_x1, px_y1), (px_x2, px_y1), (px_x2, px_y2), (px_x1, px_y2)]
                poly_str = format_pool_vector2_array(poly_pts)
                shapes_polygons.append(poly_str)

                wall_nid = self._next_node_id()
                shapes_walls.append(int(wall_nid, 16))

                walls_list.append({
                    "points": poly_str,
                    "texture": wall_texture_res,
                    "color": "ffffffff",
                    "loop": True,
                    "type": 0,
                    "joint": 1,
                    "normalize_uv": True,
                    "shadow": True,
                    "node_id": wall_nid,
                    "portals": [],
                })
                wall_pts = [(float(x), float(y)) for x, y in poly_pts]
                wall_points.append(wall_pts)
                room_polygons_pts.append(wall_pts)
                wall_loops.append(True)

        # Free wall runs. Wall zones are filtered against room perimeters to avoid double walls.
        wall_zones = [z for z in zones if z.get("kind") == "wall"]
        for wz in wall_zones:
            wx, wy = float(wz.get("x", 0)), float(wz.get("y", 0))
            ww, wh = float(wz.get("w", 1)), float(wz.get("h", 1))
            if ww <= 0 or wh <= 0:
                continue
            if ww >= wh:
                cy = (wy + wh / 2.0) * 256.0
                pts = [(wx * 256.0, cy), ((wx + ww) * 256.0, cy)]
            else:
                cx = (wx + ww / 2.0) * 256.0
                pts = [(cx, wy * 256.0), (cx, (wy + wh) * 256.0)]

            # Deduplicate wall runs that already hug a room perimeter wall
            if not is_cave and is_segment_redundant_with_rooms(pts, room_polygons_pts):
                continue

            wall_nid = self._next_node_id()
            walls_list.append({
                "points": format_pool_vector2_array(pts),
                "texture": wall_texture_res,
                "color": "ffffffff",
                "loop": False,
                "type": 1,
                "joint": 1,
                "normalize_uv": True,
                "shadow": True,
                "node_id": wall_nid,
                "portals": [],
            })
            wall_points.append(pts)
            wall_loops.append(False)

        # Cave bitmasks
        cave_bitmap_str, cave_entrance_str = generate_cave_bitmaps(cols, rows, areas, zones, is_cave)

        # Doors. Each door zone goes on the one wall segment nearest to it: a
        # door that lands on two coincident walls would be drawn twice, and a
        # door hung on the far side of the room is worse than no door at all.
        unattached_doors = []
        for dz in door_zones:
            dx, dy = float(dz.get("x", 0)), float(dz.get("y", 0))
            dw, dh = float(dz.get("w", 1)), float(dz.get("h", 1))
            cx = (dx + dw / 2.0) * 256.0
            cy = (dy + dh / 2.0) * 256.0

            best = None
            best_wall = -1
            for wi, pts in enumerate(wall_points):
                hit = nearest_point_on_polyline(pts, wall_loops[wi], cx, cy)
                if hit and (best is None or hit[0] < best[0]):
                    best, best_wall = hit, wi

            if best is None or best[0] > DOOR_SNAP_PX:
                unattached_doors.append({"id": dz.get("id", ""), "x": dx, "y": dy})
                continue

            # The planner leaves a hole in the wall where the door goes, so the
            # nearest point is often the wall's own end. A portal parked on an
            # endpoint hangs half off the wall; Dungeondraft expects the wall to
            # run through the opening and the door to cut it. So run it through.
            if not wall_loops[best_wall] and (best[2] <= 0.001 or best[2] >= 0.999):
                pts = wall_points[best_wall]
                end_i = 0 if best[2] <= 0.001 else len(pts) - 1
                ux, uy = best[6]
                ax, ay = pts[0] if end_i == 0 else pts[-2]
                along = (cx - ax) * ux + (cy - ay) * uy       # unclamped projection
                half = (dw if abs(ux) >= abs(uy) else dh) * 128.0
                reach = along - half if end_i == 0 else along + half
                pts[end_i] = (ax + ux * reach, ay + uy * reach)
                walls_list[best_wall]["points"] = format_pool_vector2_array(pts)
                best = nearest_point_on_polyline(pts, False, cx, cy)

            _dist, seg_index, t, px, py, rotation, (ux, uy) = best
            # radius is half the opening: a two-cell doorway is 256 px each way.
            radius = int(round(max(dw, dh) * 128))
            walls_list[best_wall]["portals"].append({
                "position": format_vector2(px, py),
                "rotation": round(rotation, 6),
                "scale": "Vector2( 1, 1 )",
                "direction": format_vector2(ux, uy),
                "texture": door_texture_res,
                "radius": radius,
                "point_index": seg_index,
                "wall_id": "0",
                "wall_distance": round(t, 6),
                "closed": True,
                "node_id": self._next_node_id(),
            })

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
                    "shadow": False,
                    "custom_color": "",
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
                "creation_build": "1.1.0.0 newborn phoenix",
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
                            "ambient_light": "ffe8c5c5",
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
                        "water": {
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
                                "children": [],
                            },
                        },
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
