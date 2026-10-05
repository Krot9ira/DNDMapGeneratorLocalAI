#!/usr/bin/env python3
"""Dungeondraft Assembler Regression Suite.

Runs all 13 scenes in tools/scenes/*.json through the Dungeondraft assembler,
validates format integrity, node_id monotonicity, terrain splat sizing, and
generates a comprehensive summary report.
"""
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from paths import ROOT as PROJECT_ROOT
import agent_api
from dungeondraft_assembler import DungeondraftAssembler


def validate_dungeondraft_map(map_data: dict) -> List[str]:
    """Validate format integrity of a generated .dungeondraft_map dictionary."""
    errors = []

    # 1. Top-level keys
    if "header" not in map_data:
        errors.append("Missing 'header' key")
    if "world" not in map_data:
        errors.append("Missing 'world' key")
        return errors

    world = map_data["world"]
    if world.get("format") != 3:
        errors.append(f"Invalid world.format: {world.get('format')} (expected 3)")

    width = world.get("width", 0)
    height = world.get("height", 0)
    if width <= 0 or height <= 0:
        errors.append(f"Invalid world dimensions: {width}x{height}")

    next_node_id_str = world.get("next_node_id", "0")
    try:
        next_node_id = int(next_node_id_str, 16)
    except ValueError:
        errors.append(f"next_node_id is not valid hex: {next_node_id_str}")
        next_node_id = 0

    levels = world.get("levels", {})
    if "0" not in levels:
        errors.append("Missing level '0'")
        return errors

    lvl = levels["0"]

    # 2. Tiles check
    tiles = lvl.get("tiles", {})
    cells_str = tiles.get("cells", "")
    if cells_str.startswith("PoolIntArray(") and cells_str.endswith(")"):
        inner = cells_str[len("PoolIntArray("):-1].strip()
        cell_values = [int(v.strip()) for v in inner.split(",") if v.strip()]
        expected_cells = width * height
        if len(cell_values) != expected_cells:
            errors.append(f"tiles.cells length {len(cell_values)} != width*height {expected_cells}")
    else:
        errors.append("tiles.cells is not a valid PoolIntArray")

    # 3. Terrain splat check
    terrain = lvl.get("terrain", {})
    splat_str = terrain.get("splat", "")
    if splat_str.startswith("PoolByteArray(") and splat_str.endswith(")"):
        inner = splat_str[len("PoolByteArray("):-1].strip()
        splat_values = [int(v.strip()) for v in inner.split(",") if v.strip()]
        expected_splat = (4 * width) * (4 * height) * 4
        if len(splat_values) != expected_splat:
            errors.append(f"terrain.splat byte count {len(splat_values)} != expected {expected_splat}")
    else:
        errors.append("terrain.splat is not a valid PoolByteArray")

    # 4. Node ID monotonicity and entity format
    max_used_node_id = -1

    # Walls and portals
    for w in lvl.get("walls", []):
        nid_str = w.get("node_id", "0")
        try:
            nid = int(nid_str, 16)
            max_used_node_id = max(max_used_node_id, nid)
        except ValueError:
            errors.append(f"Wall node_id '{nid_str}' is not valid hex")

        for p in w.get("portals", []):
            pnid_str = p.get("node_id", "0")
            try:
                pnid = int(pnid_str, 16)
                max_used_node_id = max(max_used_node_id, pnid)
            except ValueError:
                errors.append(f"Portal node_id '{pnid_str}' is not valid hex")

    # Objects
    for obj in lvl.get("objects", []):
        onid_str = obj.get("node_id", "0")
        try:
            onid = int(onid_str, 16)
            max_used_node_id = max(max_used_node_id, onid)
        except ValueError:
            errors.append(f"Object node_id '{onid_str}' is not valid hex")

        if not obj.get("texture"):
            errors.append("Object missing texture path")

    # Lights
    for light in lvl.get("lights", []):
        lnid_str = light.get("node_id", "0")
        try:
            lnid = int(lnid_str, 16)
            max_used_node_id = max(max_used_node_id, lnid)
        except ValueError:
            errors.append(f"Light node_id '{lnid_str}' is not valid hex")

    if max_used_node_id >= next_node_id:
        errors.append(f"world.next_node_id ({next_node_id:#x}) is not strictly greater than max used node_id ({max_used_node_id:#x})")

    errors.extend(render_problems(map_data))
    return errors


def _poly(text: str) -> List[Tuple[float, float]]:
    # Past the opening bracket: "PoolVector2Array" has a 2 in its own name.
    body = (text or "").split("(", 1)[-1]
    nums = [float(v) for v in re.findall(r"-?\d+(?:\.\d+)?", body)]
    return list(zip(nums[0::2], nums[1::2]))


def _inside(px: float, py: float, pts: List[Tuple[float, float]]) -> bool:
    inside, j = False, len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > py) != (yj > py) and px < (xj - xi) * (py - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def render_problems(map_data: dict) -> List[str]:
    """What opens without complaint but draws wrong. Each was a real map.

    Dungeondraft draws floor tiles only inside a building shape, colours tiles
    and walls from the map rather than the texture, and opens at the zoom the
    file says. Get any of those wrong and the file is valid and the map is not.
    """
    problems = []
    world = map_data.get("world", {})
    width, height = int(world.get("width", 0)), int(world.get("height", 0))
    lvl = world.get("levels", {}).get("0", {})
    tiles = lvl.get("tiles", {})
    cells = [int(v) for v in re.findall(r"-?\d+", tiles.get("cells", ""))]
    colours = tiles.get("colors") or []
    tiled = [(i % width, i // width) for i, c in enumerate(cells) if c >= 0] if width else []

    # Tiles outside every shape are never drawn.
    shapes = [_poly(t) for t in (lvl.get("shapes", {}).get("polygons") or [])]
    lost = [c for c in tiled
            if not any(_inside((c[0] + 0.5) * 256, (c[1] + 0.5) * 256, s) for s in shapes if len(s) >= 3)]
    # A wall or door square carries a tile only so the floor runs under it;
    # the part of it outside the building is allowed to go undrawn. Those sit
    # within a square or two of a building - a door at the end of a bridge is
    # two - so anything that close is not counted.
    near = [(dx, dy) for dx in (-2, -1, 0, 1, 2) for dy in (-2, -1, 0, 1, 2)]
    floor_lost = [c for c in lost
                  if not any(_inside((c[0] + 0.5 + dx) * 256, (c[1] + 0.5 + dy) * 256, s)
                             for dx, dy in near for s in shapes if len(s) >= 3)]
    if floor_lost:
        problems.append(f"{len(floor_lost)} tiled squares lie outside every building shape and "
                        f"will not be drawn (first at {floor_lost[0]})")

    # A shape with no tiles in it draws as a black hole.
    tiled_set = set(tiled)
    for n, s in enumerate(shapes):
        if len(s) < 3:
            continue
        xs = [x for x, _ in s]
        ys = [y for _, y in s]
        inner = [(cx, cy) for cy in range(int(min(ys) // 256), int(max(ys) // 256) + 1)
                 for cx in range(int(min(xs) // 256), int(max(xs) // 256) + 1)
                 if _inside((cx + 0.5) * 256, (cy + 0.5) * 256, s)]
        if inner and not (set(inner) & tiled_set):
            problems.append(f"building shape {n} holds no floor and will draw as a black hole")

    # Untinted stock tiles and walls are white.
    if tiled and all(str(colours[i // 1]).lower() == "ffffffff"
                     for i in (y * width + x for x, y in tiled) if i < len(colours)):
        problems.append("every tile is coloured ffffffff - stock tilesets draw white untinted")
    for wall in lvl.get("walls", []):
        if str(wall.get("color", "")).lower() == "ffffffff":
            problems.append("a wall is coloured ffffffff - stock walls draw white untinted")
            break

    # Opening a map zoomed right in hides it.
    zoom = float(map_data.get("header", {}).get("editor_state", {}).get("camera_zoom", 1) or 1)
    if width and zoom < min(width * 256 / 1800.0, height * 256 / 1100.0) * 0.8:
        problems.append(f"camera_zoom {zoom} opens the map zoomed in past its own edges")
    return problems


def run_checks() -> int:
    scenes_dir = PROJECT_ROOT / "tools" / "scenes"
    scene_files = sorted(scenes_dir.glob("*.json"))

    print(f"Checking Dungeondraft assembly across {len(scene_files)} scenes...\n")
    print(f"{'Scene':<20} {'Grid':<10} {'Walls':<7} {'Doors':<7} {'Objects':<9} {'No asset':<10} {'Packs':<7} {'Status'}")
    print("-" * 92)

    all_passed = True
    total_objects = 0
    total_walls = 0
    total_missing = 0
    missing_kinds: Dict[str, int] = {}

    for scene_file in scene_files:
        scene_name = scene_file.stem
        try:
            spec = json.loads(scene_file.read_text(encoding="utf-8"))
            res = agent_api.assemble_dungeondraft(
                spec,
                out_path=PROJECT_ROOT / "output" / f"scene_{scene_name}" / f"{scene_name}.dungeondraft_map",
                seed=7,
            )

            map_data = json.loads(Path(res["map_path"]).read_text(encoding="utf-8"))
            errs = validate_dungeondraft_map(map_data)

            report = res["report"]
            grid_str = f"{report['grid']['cols']}x{report['grid']['rows']}"
            walls_cnt = report["walls_placed"]
            portals_cnt = report["portals_placed"]
            objs_cnt = report["objects_placed"]
            packs_cnt = report["packs_referenced_count"]
            # What the plan asked for and the library could not answer. This is
            # the number that says which props the foundry has to generate.
            missing = report.get("unmatched_props", [])
            missing_cnt = len(missing) + report.get("doors_unattached", 0)

            total_objects += objs_cnt
            total_walls += walls_cnt
            total_missing += missing_cnt
            for m in missing:
                missing_kinds[m["kind"]] = missing_kinds.get(m["kind"], 0) + 1

            if not errs:
                status = "PASS"
            else:
                status = f"FAIL ({len(errs)} errors)"
                all_passed = False

            print(f"{scene_name:<20} {grid_str:<10} {walls_cnt:<7} {portals_cnt:<7} {objs_cnt:<9} "
                  f"{missing_cnt:<10} {packs_cnt:<7} {status}")
            if errs:
                for e in errs:
                    print(f"  -> Error: {e}")

        except Exception as exc:
            all_passed = False
            print(f"{scene_name:<20} {'-':<10} {'-':<7} {'-':<7} {'-':<9} {'-':<10} {'-':<7} EXCEPTION: {exc}")

    print("-" * 92)
    if all_passed:
        print(f"\nAll {len(scene_files)} scenes assembled and validated successfully!")
        print(f"Total walls generated: {total_walls} | Total objects placed: {total_objects} "
              f"| Plan elements with no asset: {total_missing}")
        if missing_kinds:
            worst = sorted(missing_kinds.items(), key=lambda kv: -kv[1])[:12]
            print("Nothing in the library for: " + ", ".join(f"{k} x{v}" for k, v in worst))
            print("These are what the prop foundry has to generate.")
        return 0
    else:
        print("\nSome scenes failed validation.")
        return 1


if __name__ == "__main__":
    sys.exit(run_checks())
