#!/usr/bin/env python3
"""Build every scene in tools/scenes and check the plan before any GPU time.

A plan that is wrong cannot be rendered right, and a render costs ten minutes
to discover that. This builds each scene and asks the questions a person would:
is everything reachable, is there a way in, do the things the description names
have somewhere to be, is the caption carrying them.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import ROOT  # noqa: E402

import agent_api          # noqa: E402
import architect as A     # noqa: E402
import ideogram_prompt as IP  # noqa: E402
import render_scene        # noqa: E402

WALKABLE = {A.FLOOR, A.BRIDGE, A.RUBBLE, A.VEGETATION, A.STAIRS, A.DOOR}
problems = []


def fail(scene, msg):
    problems.append(f"{scene:22} {msg}")


def check(path):
    scene = path.stem
    spec = json.loads(path.read_text(encoding="utf-8"))
    m, _spec, problems = render_scene.build_scene(spec, seed=7)
    for p in problems:
        fail(scene, p)
    b = A.border_of(m)
    m, repairs = A.validate_map(m)
    for r in repairs:
        fail(scene, r)

    g = A.zones_to_grid(m)
    cols, rows = g.cols, g.rows

    # Everything the description pins has to be somewhere a person could stand
    # or look at - an annotation over solid rock describes nothing.
    for note in m["annotations"]:
        x, y, w, h = note["x"], note["y"], note["w"], note["h"]
        if x < b or y < b or x + w > cols - b or y + h > rows - b:
            fail(scene, f"'{note['label']}' runs outside the field")
            continue
        cells = [(xx, yy) for yy in range(y, y + h) for xx in range(x, x + w)]
        void = sum(1 for (xx, yy) in cells if g.get(xx, yy) == A.VOID)
        if void == len(cells):
            fail(scene, f"'{note['label']}' sits entirely on empty space")

    # And it has to survive into the caption, or it may as well not be there.
    cap = IP.build_caption(m, agent_api.MapPlanner().load_style(m["meta"]["style"]),
                           agent_api.MapPlanner().load_base())
    text = json.dumps(cap)
    lowered = text.lower()
    for note in m["annotations"]:
        if note["label"].lower() not in lowered:
            fail(scene, f"'{note['label']}' was trimmed out of the caption")
    for area in m["areas"]:
        if area["label"] and area["label"].lower() not in lowered:
            fail(scene, f"room '{area['label']}' was trimmed out of the caption")

    # A scene may not ask for something that only exists on a vertical surface
    # or above the floor: the renderer can only show it by drawing the wall from
    # the side, and the whole picture tips into perspective to fit it in. The
    # same rule the styles are held to.
    for note in m["annotations"] + [dict(a) for a in m["areas"]]:
        body = (str(note.get("description", "")) + " " + str(note.get("label", ""))).lower()
        for phrase in IP._SIDE_ON_WORDS:
            if phrase in body:
                fail(scene, f"'{note.get('label', '?')}' says '{phrase}' - nothing can be "
                            f"shown on a wall or overhead from directly above")
                break

    els = cap["compositional_deconstruction"]["elements"]
    return scene, len(m["areas"]), len(m["annotations"]), len(els)


scenes = sorted((ROOT / "tools" / "scenes").glob("*.json"))
print(f"checking {len(scenes)} scenes\n")
print(f"{'scene':22} {'rooms':>5} {'pinned':>6} {'elements':>8}")
for path in scenes:
    name, areas, notes, els = check(path)
    print(f"{name:22} {areas:5} {notes:6} {els:8}")

print()
if problems:
    print(f"{len(problems)} problems:")
    for line in problems:
        print("  -", line)
else:
    print("every scene plans cleanly")
