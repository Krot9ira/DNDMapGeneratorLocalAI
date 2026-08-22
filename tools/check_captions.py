#!/usr/bin/env python3
"""Read the caption for every style and look for what spoils a render.

Not "does it crash" - the caption always builds. This looks for the things that
quietly cost quality: a rectangle that says nothing, a phrase repeated twice in
one breath, coordinates outside the frame, a style with an empty field, a
sentence that contradicts the one before it.
"""
import json
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import ROOT  # noqa: E402

import architect as A            # noqa: E402
import ideogram_prompt as IP     # noqa: E402
from planner import MapPlanner   # noqa: E402

planner = MapPlanner()
problems = []


def fail(where, msg):
    problems.append(f"{where:28} {msg}")


def check_caption(where, cap, cols, rows, map_data=None):
    head = cap.get("high_level_description", "")
    if not head:
        fail(where, "no high-level description at all")
    if "  " in head:
        fail(where, "double space in the description")
    for bad in (" ,", " .", ",.", "..", ",,"):
        if bad in head:
            fail(where, f"stray punctuation {bad!r} in the description")

    sd = cap.get("style_description", {})
    for key in ("aesthetics", "lighting", "medium"):
        if not str(sd.get(key, "")).strip():
            fail(where, f"style_description.{key} is empty")
    if not sd.get("color_palette"):
        fail(where, "no colour palette")

    cd = cap.get("compositional_deconstruction", {})
    bg = cd.get("background", "")
    if not bg.strip():
        fail(where, "empty background")
    for bad in (" ,", ",.", "..", "  "):
        if bad in bg:
            fail(where, f"stray punctuation {bad!r} in the background")

    els = cd.get("elements", [])
    if not els:
        fail(where, "no elements")
    budget = IP.element_budget(map_data) if map_data else IP.MAX_ELEMENTS
    if len(els) > budget + 1:               # +1 for the un-bboxed clutter line
        fail(where, f"{len(els)} elements, over the budget of {budget}")

    seen = Counter()
    for i, e in enumerate(els):
        desc = str(e.get("desc", ""))
        if not desc.strip():
            fail(where, f"element {i} has no description")
            continue
        for bad in (" ,", " .", ",.", "..", "  ", ",,"):
            if bad in desc:
                fail(where, f"element {i} has stray punctuation {bad!r}")
                break
        # The same sentence twice in one description reads as emphasis to a
        # person and as noise to the renderer.
        sentences = [s.strip().lower() for s in re.split(r"(?<=[.!?]) ", desc) if s.strip()]
        dupes = [s for s, n in Counter(sentences).items() if n > 1 and len(s) > 25]
        if dupes:
            fail(where, f"element {i} repeats a sentence: {dupes[0][:60]}...")
        seen[desc] += 1

        if "bbox" not in e:
            continue
        box = e["bbox"]
        if not (isinstance(box, list) and len(box) == 4):
            fail(where, f"element {i} has a malformed bbox {box}")
            continue
        y1, x1, y2, x2 = box
        if not all(0 <= v <= 1000 for v in box):
            fail(where, f"element {i} bbox outside 0-1000: {box}")
        if y2 <= y1 or x2 <= x1:
            fail(where, f"element {i} bbox has no area: {box}")
        # A box covering nearly everything tells the renderer nothing about
        # where the thing is, which is the whole point of the format.
        covers = (y2 - y1) * (x2 - x1) / 1e6
        if covers > 0.92 and not desc.startswith(("One single", "The ")):
            fail(where, f"element {i} covers {covers:.0%} of the frame: {desc[:50]}")

    for text, n in seen.items():
        limit = 4 if text.startswith(("Open ground", "A mass of", "A body of")) else 3
        if n > limit:
            fail(where, f"{n} elements say exactly the same thing: {text[:50]}...")


styles = planner.load_styles()
print(f"checking {len(styles)} styles")

for sid, style in sorted(styles.items()):
    for field in ("name", "materials", "ground", "aesthetics", "lighting", "default_layout"):
        if not str(style.get(field, "")).strip():
            fail(sid, f"style field '{field}' is empty")
    layout = style.get("default_layout", "dungeon")
    if layout not in A.LAYOUTS:
        fail(sid, f"default_layout '{layout}' is not a layout")
        layout = "dungeon"

    m = A.build({"name": sid, "style": sid, "layout": layout,
                 "grid": {"cols": 34, "rows": 26},
                 "scene_summary": "a place to fight in", "prop_density": "medium"}, seed=9)
    cap = IP.build_caption(m, style, planner.load_base())
    check_caption(sid, cap, 34, 26, m)

# One map with everything on it, since annotations and effects take their own
# route into the caption and are easy to break without noticing.
m = A.build({"name": "loaded", "style": "gothic_crypt", "layout": "dungeon",
             "grid": {"cols": 30, "rows": 24}, "scene_summary": "a crypt",
             "prop_density": "medium"}, seed=3)
b = A.border_of(m)
m["annotations"].append({"label": "Barred Gate", "description": "An iron portcullis.",
                         "elaboration": "exact", "x": b + 4, "y": b + 4, "w": 3, "h": 1})
m["effects"].append({"kind": "fog", "label": "", "description": "", "elaboration": "some",
                     "intensity": "high", "x": b + 2, "y": b + 2, "w": 10, "h": 8})
m["features"].append({"kind": "raspberry_bush", "x": b + 6, "y": b + 6, "structural": True})
cap = IP.build_caption(m, planner.load_style("gothic_crypt"), planner.load_base())
check_caption("loaded map", cap, 30, 24)
text = json.dumps(cap)
if "Barred Gate" not in text:
    fail("loaded map", "an annotation did not reach the caption")
if "fog" not in text.lower():
    fail("loaded map", "an effect did not reach the caption")

print()
if problems:
    print(f"{len(problems)} problems:")
    for line in problems:
        print("  -", line)
else:
    print("no problems found")
