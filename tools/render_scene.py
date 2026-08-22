#!/usr/bin/env python3
"""Build and render one scene file: a spec plus the things it pins by hand.

    python tools/render_scene.py tools/scenes/tavern.json [--plan-only]

A scene file is an ordinary agent spec with one addition: an `annotations`
list, in field coordinates, for everything the description puts in a
particular place. The bleed margin is added afterwards, so the coordinates in
the file are the ones a person would read off the plan.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import agent_api          # noqa: E402
import architect as A     # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scene")
    ap.add_argument("--plan-only", action="store_true",
                    help="build the plan and stop, without rendering")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out", help="output directory")
    args = ap.parse_args()

    spec = json.loads(Path(args.scene).read_text(encoding="utf-8"))
    pinned = spec.pop("annotations", [])
    name = spec.get("name") or Path(args.scene).stem
    out = args.out or str(A.PROJECT / "output" / name) if hasattr(A, "PROJECT") else None

    built = agent_api.build_map(spec, seed=args.seed)
    m = built["map_json"]
    for p in built.get("problems", []):
        print("  !", p)

    # Hand-pinned things are written in field coordinates; the stored map counts
    # from the outside of the bleed margin.
    b = A.border_of(m)
    for note in pinned:
        m["annotations"].append({
            "label": note["label"],
            "description": note["description"],
            "elaboration": note.get("elaboration", "exact"),
            "x": note["x"] + b, "y": note["y"] + b,
            "w": note["w"], "h": note["h"],
        })
    A.validate_map(m)

    out_dir = args.out or str(Path(agent_api.PROJECT) / "output" / name)
    if args.plan_only:
        agent_api.write_blueprint(m, out_dir, spec=spec, caption=agent_api.make_caption(m))
        print("plan:", out_dir)
        return

    result = agent_api.generate_from_map(m, out_dir=out_dir, seed=args.seed,
                                         on_progress=lambda n: print("  ", n))
    print("images:", result["images"])


if __name__ == "__main__":
    main()
