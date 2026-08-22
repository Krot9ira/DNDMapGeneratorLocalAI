"""Compare the caption the app builds with the one the tools build.

Two ports of the same builder, every fix copied across by hand. A difference
means a map made in the app renders differently from the same map made by an
agent, which is the sort of thing nobody notices until the pictures disagree.
"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(r"W:\ai\Projects\DNDMapGeneratorLocalAI")
sys.path.insert(0, str(ROOT / "tools"))

import architect as A            # noqa: E402
import ideogram_prompt as IP     # noqa: E402
from planner import MapPlanner   # noqa: E402

# The development build, or the executable sitting in an unpacked release.
EXE = ROOT / "bin" / "Release" / "DndBattlemapGenerator.exe"
if not EXE.exists():
    EXE = ROOT / "DndBattlemapGenerator.exe"
TMP = ROOT / "output" / "_parity"
TMP.mkdir(parents=True, exist_ok=True)

CASES = [
    ("harbour", "city_harbour"),
    ("building", "city_townhouse"),
    ("dungeon", "gothic_crypt"),
    ("cavern", "underdark_cavern"),
    ("forest", "lush_forest"),
    ("street", "village_hamlet"),
    ("district", "city_district"),
    ("deck", "pirate_deck"),
    ("swamp", "marsh_bog"),
    ("arena", "grand_temple"),
]

planner = MapPlanner()
problems = []

for layout, style in CASES:
    m = A.build({"name": "parity", "style": style, "layout": layout,
                 "grid": {"cols": 34, "rows": 26}, "scene_summary": "a place to fight in",
                 "prop_density": "medium"}, seed=5)
    map_path = TMP / f"{layout}.json"
    map_path.write_text(json.dumps(m, indent=2, ensure_ascii=False), encoding="utf-8")

    py = IP.build_caption(m, planner.load_style(style), planner.load_base())

    out = TMP / f"{layout}.app.json"
    if out.exists():
        out.unlink()
    rc = subprocess.run([str(EXE), "--caption", str(map_path), str(out)],
                        capture_output=True).returncode
    if rc != 0 or not out.exists():
        problems.append(f"{layout}: the app could not produce a caption (exit {rc})")
        continue
    app = json.loads(out.read_text(encoding="utf-8"))

    for key in ("aspect_ratio", "high_level_description"):
        if py.get(key) != app.get(key):
            problems.append(f"{layout}: {key} differs")
            if key == "high_level_description":
                a, b = str(py.get(key)), str(app.get(key))
                for i, (ca, cb) in enumerate(zip(a, b)):
                    if ca != cb:
                        problems.append(f"    tools: ...{a[max(0,i-40):i+70]}")
                        problems.append(f"    app  : ...{b[max(0,i-40):i+70]}")
                        break
                else:
                    problems.append(f"    tools ends: ...{a[len(b):][:90]}"
                                    if len(a) > len(b) else
                                    f"    app ends:   ...{b[len(a):][:90]}")

    ps = py["style_description"]
    as_ = app["style_description"]
    for key in ("aesthetics", "lighting", "medium"):
        if ps.get(key) != as_.get(key):
            problems.append(f"{layout}: style_description.{key} differs")

    pd = py["compositional_deconstruction"]
    ad = app["compositional_deconstruction"]
    if pd["background"] != ad["background"]:
        problems.append(f"{layout}: background differs")
        a, b = pd["background"], ad["background"]
        for i, (ca, cb) in enumerate(zip(a, b)):
            if ca != cb:
                problems.append(f"    tools: ...{a[max(0,i-30):i+70]}")
                problems.append(f"    app  : ...{b[max(0,i-30):i+70]}")
                break

    pe, ae = pd["elements"], ad["elements"]
    if len(pe) != len(ae):
        problems.append(f"{layout}: {len(pe)} elements from the tools, {len(ae)} from the app")
    for i, (x, y) in enumerate(zip(pe, ae)):
        if x.get("bbox") != y.get("bbox"):
            problems.append(f"{layout}: element {i} bbox {x.get('bbox')} vs {y.get('bbox')}")
            break
        if x.get("desc") != y.get("desc"):
            problems.append(f"{layout}: element {i} desc differs")
            problems.append(f"    tools: {str(x.get('desc'))[:110]}")
            problems.append(f"    app  : {str(y.get('desc'))[:110]}")
            break

print(f"compared {len(CASES)} layouts")
if problems:
    print(f"\n{len(problems)} differences:")
    for line in problems:
        print(" ", line)
else:
    print("\nthe app and the tools build the same caption")
