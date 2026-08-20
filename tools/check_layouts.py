"""Check every generator against what its name claims, not just that it runs.

Several sizes, several seeds, and one invariant per layout, plus the checks that
apply to all of them: everything reachable, nothing placed inside a wall, doors
only inside wall runs, areas inside the field, and the app's dispatch carrying a
branch for every layout the library offers.
"""
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(r"W:\ai\Projects\DNDMapGeneratorLocalAI")
sys.path.insert(0, str(ROOT / "tools"))

import architect as A  # noqa: E402

sys.exit_code = 0

SIZES = [(12, 10), (25, 19), (48, 36), (90, 70)]
SEEDS = [1, 7, 23]
WALKABLE = {A.FLOOR, A.BRIDGE, A.RUBBLE, A.VEGETATION, A.STAIRS, A.DOOR}

problems = []


def fail(layout, size, seed, msg):
    problems.append(f"{layout:8} {size[0]:>3}x{size[1]:<3} seed {seed:<3} {msg}")


def longest_run(grid, kind):
    """Longest straight run of one tile kind, horizontal or vertical."""
    best = 0
    for y in range(grid.rows):
        run = 0
        for x in range(grid.cols):
            run = run + 1 if grid.get(x, y) == kind else 0
            best = max(best, run)
    for x in range(grid.cols):
        run = 0
        for y in range(grid.rows):
            run = run + 1 if grid.get(x, y) == kind else 0
            best = max(best, run)
    return best


def check(layout, size, seed, rooms_wanted=None):
    cols, rows = size
    spec = {"name": layout, "style": "classic_dungeon", "layout": layout,
            "grid": {"cols": cols, "rows": rows}, "scene_summary": "audit",
            "prop_density": "medium"}
    # How many areas the caller asks for is its own axis: a layout that only
    # works with three or four rooms is broken for anybody who asks for one.
    if rooms_wanted is not None:
        spec["rooms"] = [{"label": f"Area {i + 1}", "size": "m", "props": []}
                         for i in range(rooms_wanted)]
    try:
        m = A.build(spec, seed=seed)
    except Exception as exc:                       # noqa: BLE001
        fail(layout, size, seed, f"raised {type(exc).__name__}: {exc}")
        return None

    g = A.zones_to_grid(m)
    b = A.border_of(m)
    counts = Counter(g.get(x, y) for y in range(g.rows) for x in range(g.cols))
    play = (g.cols - 2 * b) * (g.rows - 2 * b)
    walk = sum(counts[k] for k in WALKABLE)
    share = walk / max(1, play)

    # -- universal ----------------------------------------------------
    if walk == 0:
        fail(layout, size, seed, "nothing walkable at all")
        return m
    if share < 0.12:
        fail(layout, size, seed, f"only {share:.0%} of the field is walkable")
    biggest = A._largest_component(g, WALKABLE)
    if len(biggest) < walk:
        fail(layout, size, seed,
             f"{walk - len(biggest)} walkable cells unreachable ({len(biggest)}/{walk})")
    if not m["areas"]:
        fail(layout, size, seed, "no areas, so props and the caption have nothing to name")
    for a in m["areas"]:
        if not (0 <= a["x"] < g.cols and 0 <= a["y"] < g.rows):
            fail(layout, size, seed, f"area '{a['label']}' starts outside the grid")
        if a["x"] + a["w"] > g.cols or a["y"] + a["h"] > g.rows:
            fail(layout, size, seed, f"area '{a['label']}' runs off the grid")
    for f in m["features"]:
        if not isinstance(f.get("kind"), str) or not f["kind"].strip():
            fail(layout, size, seed, f"a prop has {f.get('kind')!r} for a name")
            break
        t = g.get(f["x"], f["y"])
        if t in (A.WALL, A.VOID, A.WATER, A.PIT):
            fail(layout, size, seed, f"prop '{f['kind']}' sits on {t}")
            break
        if b and not (b <= f["x"] < g.cols - b and b <= f["y"] < g.rows - b):
            fail(layout, size, seed, f"prop '{f['kind']}' is in the bleed margin")
            break
    for y in range(g.rows):
        for x in range(g.cols):
            if g.get(x, y) != A.DOOR:
                continue
            lr = (g.get(x - 1, y), g.get(x + 1, y))
            ud = (g.get(x, y - 1), g.get(x, y + 1))
            if not (lr == (A.WALL, A.WALL) or ud == (A.WALL, A.WALL)):
                fail(layout, size, seed, f"door at {x},{y} is not seated in a wall")
                break
        else:
            continue
        break

    # -- what the name promises --------------------------------------
    veg = counts[A.VEGETATION] / max(1, play)
    water = counts[A.WATER] / max(1, play)
    wall = counts[A.WALL] / max(1, play)
    if layout == "forest" and veg < 0.25:
        fail(layout, size, seed, f"a forest with {veg:.0%} undergrowth")
    if layout == "swamp":
        if water < 0.20:
            fail(layout, size, seed, f"a swamp with {water:.0%} water")
    if layout == "deck":
        if water < 0.15:
            fail(layout, size, seed, f"a ship at sea with {water:.0%} water")
        if not any(s.get("kind") == "ship" for s in m.get("structures", [])):
            fail(layout, size, seed, "no ship on a ship deck")
        if counts[A.BRIDGE] == 0:
            fail(layout, size, seed, "no decking")
    if layout == "harbour":
        if water < 0.10:
            fail(layout, size, seed, f"a harbour with {water:.0%} water")
        if not any(s.get("kind") == "ship" for s in m.get("structures", [])):
            fail(layout, size, seed, "a harbour with no ship")
    if layout == "cavern":
        if counts[A.DOOR]:
            fail(layout, size, seed, "a cave with doors in it")
        skin = b + 3
        inner = A.TileGrid(g.cols, g.rows, A.VOID)
        for yy in range(skin, g.rows - skin):
            for xx in range(skin, g.cols - skin):
                inner.set(xx, yy, g.get(xx, yy))
        run = longest_run(inner, A.WALL)
        if g.cols >= 24 and run > max(8, g.cols // 3):
            fail(layout, size, seed, f"a cave with a {run}-cell straight wall")
    if layout in ("dungeon", "building") and counts[A.DOOR] == 0 and len(m["areas"]) > 1:
        fail(layout, size, seed, "separate areas with no door between them")
    if layout == "open" and wall > 0.04:
        fail(layout, size, seed, f"open ground that is {wall:.0%} wall")
    if layout == "ruins" and counts[A.WALL] == 0:
        fail(layout, size, seed, "ruins with nothing ruined")
    return m


print("checking", len(A.LAYOUTS) - 1, "layouts x", len(SIZES), "sizes x", len(SEEDS), "seeds")
fingerprints = {}
for layout in A.LAYOUTS:
    if layout == "custom":
        continue
    for size in SIZES:
        for seed in SEEDS:
            m = check(layout, size, seed)
            for want in (1, 2, 7):
                check(layout, size, seed, rooms_wanted=want)
            if m is not None and size == (48, 36) and seed == 7:
                g = A.zones_to_grid(m)
                c = Counter(g.get(x, y) for y in range(g.rows) for x in range(g.cols))
                total = sum(c.values())
                fingerprints[layout] = {k: round(v / total, 3) for k, v in c.items()}

# -- near-duplicate layouts ------------------------------------------
names = sorted(fingerprints)
for i, a in enumerate(names):
    for bname in names[i + 1:]:
        keys = set(fingerprints[a]) | set(fingerprints[bname])
        diff = sum(abs(fingerprints[a].get(k, 0) - fingerprints[bname].get(k, 0)) for k in keys)
        if diff < 0.12:
            problems.append(f"{a} and {bname} are nearly indistinguishable (diff {diff:.2f})")

# -- does the app know about all of them? ----------------------------
cpp = (ROOT / "app/include/map_architect.h").read_text(encoding="utf-8")
branch = set(re.findall(r'L == "([a-z]+)"', cpp))
for layout in A.LAYOUTS:
    if layout in ("custom", "dungeon"):
        continue                                   # dungeon is the else branch
    if layout not in branch:
        problems.append(f"the app has no generator for '{layout}' - it falls back to dungeon")

print()
if problems:
    print(len(problems), "problems:")
    for line in problems:
        print("  -", line)
else:
    print("no problems found")
