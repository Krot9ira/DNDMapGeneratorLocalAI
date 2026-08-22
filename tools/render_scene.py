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


def build_scene(spec, seed=7):
    """Turn a scene file into a finished map.

    A scene file is an ordinary agent spec plus `annotations` and `effects`
    written in field coordinates - the ones a person reads off the plan. The
    bleed margin is added by the architect, so they are shifted here to match.
    Both the checker and the renderer come through this function; when they had
    one each, a scene could pass the check and render as a different map.
    """
    spec = dict(spec)
    spec["effects"] = [{"kind": fx["kind"], "intensity": fx.get("strength", "medium"),
                        "x": fx["x"], "y": fx["y"], "w": fx["w"], "h": fx["h"]}
                       for fx in (spec.get("effects") or [])]
    built = agent_api.build_map(spec, seed=seed)
    return built["map_json"], spec, built.get("problems", [])


def run_one(path, args):
    raw = json.loads(Path(path).read_text(encoding="utf-8"))
    name = raw.get("name") or Path(args.scene).stem
    m, spec, problems = build_scene(raw, seed=args.seed)
    for p in problems:
        print("  !", p)
    A.validate_map(m)

    out_dir = args.out or str(Path(agent_api.PROJECT) / "output" / name)
    if args.plan_only:
        agent_api.write_blueprint(m, out_dir, spec=spec, caption=agent_api.make_caption(m))
        print("plan:", out_dir)
        return

    result = agent_api.generate_from_map(m, out_dir=out_dir, seed=args.seed,
                                         on_progress=lambda n: print("  ", n))
    print("images:", result["images"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scene", nargs="?",
                    help="a scene file; omit it and pass --all for the whole suite")
    ap.add_argument("--all", action="store_true",
                    help="render every scene in tools/scenes, one after another")
    ap.add_argument("--plan-only", action="store_true",
                    help="build the plan and stop, without rendering")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out", help="output directory")
    args = ap.parse_args()

    if args.all:
        scenes = sorted((Path(__file__).resolve().parent / "scenes").glob("*.json"))
        for i, path in enumerate(scenes, 1):
            print(f"[{i}/{len(scenes)}] {path.stem}")
            run_one(path, args)
        return
    if not args.scene:
        ap.error("give a scene file, or --all")
    run_one(args.scene, args)


if __name__ == "__main__":
    main()
