#!/usr/bin/env python3
"""
D&D battle map pipeline.

  Stage 1 (brain)     description or sketch -> design spec -> validated geometry
  Stage 2 (renderer)  geometry -> JSON caption with bounding boxes -> Ideogram 4

Usage:
  python pipeline.py styles [--json]
  python pipeline.py plan "scene description" [--style ID] [--size medium] [--out DIR]
  python pipeline.py build spec.json [--out DIR]        # spec -> map, no LLM
  python pipeline.py preview map.json [--out DIR]       # redraw the human preview
  python pipeline.py generate map.json [--seed N]
  python pipeline.py auto "scene description" [--style ID] [--size medium]
  python pipeline.py validate map.json
"""
import argparse
import json
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (ValueError, OSError):
        pass

from paths import ROOT as PROJECT
sys.path.insert(0, str(PROJECT))

import agent_api                          # noqa: E402
import architect as A                     # noqa: E402
from comfy import ComfyClient             # noqa: E402
from ollama_client import OllamaClient    # noqa: E402
from planner import MapPlanner            # noqa: E402
from workflow import build_ideogram4      # noqa: E402


def load_config():
    path = PROJECT / "config.json"
    if path.exists():
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except ValueError as exc:
            print(f"[warn] config.json is not valid JSON ({exc}); using defaults")
    return {}


def make_planner(cfg):
    oc = cfg.get("ollama", {})
    return MapPlanner(ollama_client=OllamaClient(
        base_url=oc.get("base_url", "http://127.0.0.1:11434"),
        model=oc.get("model", "qwen3.8:27b"),
        timeout=int(oc.get("timeout", 600))))


def load_json(path):
    path = Path(path)
    if not path.exists():
        sys.exit(f"[error] {path} not found")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except ValueError as exc:
        sys.exit(f"[error] {path} is not valid JSON: {exc}")


def out_dir_for(args, name):
    return Path(args.out) if getattr(args, "out", None) else PROJECT / "output" / name


def write_stage1(map_data, out_dir, spec=None):
    """Write every Stage 1 artefact and report what went where."""
    caption = agent_api.make_caption(map_data)
    out_dir = agent_api.write_blueprint(map_data, out_dir, spec=spec, caption=caption)
    grid = map_data["grid"]
    print(f"[stage 1] map.json     {out_dir / 'map.json'}")
    print(f"[stage 1] preview.png  {out_dir / 'preview.png'}  ({grid['cols']}x{grid['rows']} cells)")
    print(f"[stage 1] caption.json {out_dir / 'caption.json'}  ({len(caption)} chars, text-free)")
    return caption


def cmd_styles(args):
    styles = MapPlanner().load_styles()
    if args.json:
        print(json.dumps(styles, indent=2, ensure_ascii=False))
        return
    print("\nAvailable styles:")
    for sid, s in sorted(styles.items()):
        print(f"  {sid:<18} {s.get('name', sid):<28} layout={s.get('default_layout', '-'):<9}"
              f" {', '.join(s.get('tags', [])[:4])}")
    print(f"\n  layouts: {', '.join(A.LAYOUTS)}")
    print(f"  sizes:   {', '.join(f'{k} ({v[0]}x{v[1]})' for k, v in A.SIZE_PRESETS.items())}\n")


def cmd_plan(args):
    cfg = load_config()
    planner = make_planner(cfg)
    size = args.size or cfg.get("default_size", "medium")

    print(f"[stage 1] planning with {planner.client.model}")
    print(f"          scene: {args.description}")
    print(f"          style: {args.style or '(model picks)'} | size: {size}")
    result = planner.plan_map(args.description, style_id=args.style, size=size,
                              seed=args.seed, sketch_path=args.sketch,
                              cols=args.cols, rows=args.rows)
    map_data = result["map_json"]
    print(f"          layout: {result['spec'].get('layout')} | areas: "
          f"{', '.join(a['label'] for a in map_data.get('areas', []))}")
    write_stage1(map_data, out_dir_for(args, map_data["meta"]["name"]), spec=result["spec"])
    return map_data


def cmd_build(args):
    """Spec -> map without touching the LLM. This is the agent path."""
    result = MapPlanner().compose(load_json(args.spec), seed=args.seed,
                                  cols=args.cols, rows=args.rows)
    map_data = result["map_json"]
    write_stage1(map_data, out_dir_for(args, map_data["meta"]["name"]), spec=result["spec"])


def cmd_preview(args):
    map_data, problems = A.validate_map(load_json(args.map))
    for p in problems:
        print(f"[repair] {p}")
    write_stage1(map_data, out_dir_for(args, map_data["meta"]["name"]))


def cmd_generate(args, map_data=None):
    if map_data is None:
        map_data, problems = A.validate_map(load_json(args.map))
        for p in problems:
            print(f"[repair] {p}")

    cfg = load_config()
    client = ComfyClient(cfg.get("comfy", {}).get("base_url", "http://127.0.0.1:8188"))
    ok, detail = client.health()
    if not ok:
        sys.exit(f"[error] ComfyUI is not reachable: {detail}\n"
                 f"        Start ComfyUI, then run this again.")
    print(f"[stage 2] {detail}")

    out = out_dir_for(args, map_data["meta"]["name"])
    result = agent_api.generate(map_data=map_data, seed=args.seed, out_dir=out,
                               timeout=args.timeout,
                               on_progress=lambda msg: print(f"          {msg}", flush=True))
    print(f"[stage 2] {result['size'][0]}x{result['size'][1]} px")
    for path in result["images"]:
        print(f"[stage 2] battle map: {path}")
    return result["images"]


def cmd_auto(args):
    map_data = cmd_plan(args)
    print("\n[stage 2] starting render (Ollama has finished, VRAM is free)\n")
    cmd_generate(args, map_data=map_data)


def cmd_caption(args):
    """Print exactly what the renderer would be told for a plan, and stop.

    The same thing the app writes with --caption, so the two can be compared
    when a render does not match the plan.
    """
    text = agent_api.make_caption(load_json(args.map))
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
        print(f"[caption] wrote {args.out}")
    else:
        print(text)


def cmd_validate(args):
    data, problems = A.validate_map(load_json(args.map))
    grid = A.zones_to_grid(data)
    walkable = sum(grid.count(k) for k in A.WALKABLE)
    if problems:
        print("[validation] repaired:")
        for p in problems:
            print(f"  - {p}")
    else:
        print("[validation] clean")
    print(f"  grid      {data['grid']['cols']}x{data['grid']['rows']}")
    print(f"  zones     {len(data['zones'])}")
    print(f"  features  {len(data['features'])}")
    print(f"  walkable  {walkable} cells ({walkable * 100 // (grid.cols * grid.rows)}%)")


def main():
    parser = argparse.ArgumentParser(description="D&D AI battle map generator")
    sub = parser.add_subparsers(dest="command", required=True)

    def add_common(p):
        p.add_argument("--out", help="output directory")
        p.add_argument("--seed", type=int, default=None, help="deterministic seed")

    p = sub.add_parser("styles", help="list styles, layouts and sizes")
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("plan", help="stage 1 via the local LLM")
    p.add_argument("description")
    p.add_argument("--style")
    p.add_argument("--size", choices=sorted(A.SIZE_PRESETS))
    p.add_argument("--sketch", help="path to a layout sketch image")
    p.add_argument("--cols", type=int)
    p.add_argument("--rows", type=int)
    add_common(p)

    p = sub.add_parser("build", help="stage 1 from a spec file, no LLM (agent path)")
    p.add_argument("spec")
    p.add_argument("--cols", type=int)
    p.add_argument("--rows", type=int)
    add_common(p)

    p = sub.add_parser("preview", help="redraw the human preview from a map.json")
    p.add_argument("map")
    add_common(p)

    p = sub.add_parser("generate", help="stage 2 via Ideogram 4 in ComfyUI")
    p.add_argument("map")
    p.add_argument("--timeout", type=int, default=2400)
    add_common(p)

    p = sub.add_parser("auto", help="stage 1 then stage 2, sequentially")
    p.add_argument("description")
    p.add_argument("--style")
    p.add_argument("--size", choices=sorted(A.SIZE_PRESETS))
    p.add_argument("--sketch")
    p.add_argument("--cols", type=int)
    p.add_argument("--rows", type=int)
    p.add_argument("--timeout", type=int, default=2400)
    add_common(p)

    p = sub.add_parser("caption", help="print what the renderer would be told")
    p.add_argument("map")
    p.add_argument("--out", help="write it to a file instead of the screen")

    p = sub.add_parser("validate", help="check and repair a map.json")
    p.add_argument("map")

    args = parser.parse_args()
    {"styles": cmd_styles, "plan": cmd_plan, "build": cmd_build, "preview": cmd_preview,
     "generate": cmd_generate, "auto": cmd_auto, "caption": cmd_caption,
     "validate": cmd_validate}[args.command](args)


if __name__ == "__main__":
    main()
