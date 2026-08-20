#!/usr/bin/env python3
"""
Python API for AI agents.

An agent that is already a capable language model does not need the local LLM at
all: it writes the design spec itself. That skips Ollama entirely, frees the
VRAM for the renderer, and is the fastest path.

TWO CALLS, AND THEY ARE NOT INTERCHANGEABLE
-------------------------------------------

    blueprint(spec)  -> the plan only. Seconds, no GPU. Writes map.json and a
                        readable preview.png. This is what "make me a schema",
                        "a layout", "a floor plan", "a blueprint" means.

    generate(spec)   -> the finished painted map. Minutes, needs ComfyUI
                        running with the Ideogram 4 models loaded.

If the request is ambiguous, call `blueprint`. It is cheap and reversible, and
its output can be rendered afterwards with `generate_from_map(...)` without
redoing the planning. Rendering four maps because somebody asked for four
layouts is the expensive mistake.

    import agent_api

    plan = agent_api.blueprint({
        "title": "Baldur's Gate Docks",
        "style": "city_harbour",
        "layout": "harbour",
        "size": "large",
        "scene_summary": "A stone quay with a large ship moored alongside.",
        "rooms": [
            {"label": "Warehouse",     "size": "m", "props": ["crate", "barrel"]},
            {"label": "Harbourmaster", "size": "s", "props": ["table"]},
        ],
    })
    print(plan["out_dir"])          # map.json + preview.png live here

    # ...and only when a finished image is actually wanted:
    agent_api.generate_from_map(plan["map_json"])

Full field reference: AGENTS.md, next to this folder.
"""
import json
import math
from pathlib import Path

import architect as A
from comfy import ComfyClient
from ideogram_prompt import build_caption_json
from ollama_client import OllamaClient
from planner import MapPlanner
from render import render_preview, render_svg, trim_to_margin
from ideogram_prompt import style_warnings
from workflow import build_ideogram4

from paths import ROOT as PROJECT


def load_config():
    path = PROJECT / "config.json"
    if path.exists():
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except ValueError:
            pass
    return {}


def list_styles():
    """Every available style, keyed by id."""
    return MapPlanner().load_styles()


def list_layouts():
    """Layout generators and the grid sizes they can be built at."""
    return {"layouts": A.LAYOUTS,
            "sizes": {k: {"cols": v[0], "rows": v[1]} for k, v in A.SIZE_PRESETS.items()}}


def build_map(spec, seed=None, cols=None, rows=None):
    """Spec -> map dict + caption inputs. No network calls, no LLM."""
    planner = MapPlanner()
    result = planner.compose(spec, seed=seed, cols=cols, rows=rows)
    # A style whose own text argues with the plan beats anything the caption
    # says about it, so it is worth saying out loud before the GPU is spent.
    try:
        style = planner.load_style(result["map_json"]["meta"].get("style", ""))
        warnings = style_warnings(result["map_json"], style)
        if warnings:
            result.setdefault("problems", []).extend(warnings)
    except Exception:
        pass
    return result


def target_size(map_data, megapixels=1.8):
    """Pixel size matching the map's cell aspect, snapped to a multiple of 16."""
    grid = map_data.get("grid", {}) or {}
    cols = max(1, int(grid.get("cols", 25)))
    rows = max(1, int(grid.get("rows", 19)))
    scale = math.sqrt(megapixels * 1_000_000 / (cols * rows))

    def snap(value):
        return max(256, int(round(value / 16.0)) * 16)

    return snap(cols * scale), snap(rows * scale)


def write_blueprint(map_data, out_dir, spec=None, caption=None):
    """Write map.json, the human preview and the caption actually sent."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "map.json").write_text(json.dumps(map_data, indent=2, ensure_ascii=False),
                                      encoding="utf-8")
    if spec is not None:
        (out_dir / "spec.json").write_text(json.dumps(spec, indent=2, ensure_ascii=False),
                                           encoding="utf-8")
    if caption is not None:
        (out_dir / "caption.json").write_text(caption, encoding="utf-8")
    render_preview(map_data, out_dir / "preview.png")
    render_svg(map_data, out_dir / "preview.svg")
    return out_dir


def make_caption(map_data):
    """The structured JSON caption that carries the layout as bounding boxes."""
    planner = MapPlanner()
    style = planner.load_style(map_data.get("meta", {}).get("style"))
    return build_caption_json(map_data, style, planner.load_base())


def blueprint(spec, seed=None, out_dir=None, cols=None, rows=None):
    """Stage 1 only: plan the place, write the blueprint, and stop.

    No GPU, no image, seconds rather than minutes. Writes `map.json` (the plan
    itself), `preview.png` (a labelled picture of it a human can read) and
    `caption.json` (what the painter would be told). Open the result in the
    desktop app with File - Open map.json, or hand it to `generate` later.

    This is the one to call when somebody asks for a plan, a layout, a
    blueprint or a schema. `generate` paints a finished map and takes minutes.
    """
    result = build_map(spec, seed=seed, cols=cols, rows=rows)
    map_data = result["map_json"]
    out_dir = Path(out_dir) if out_dir else PROJECT / "output" / map_data["meta"]["name"]
    caption = make_caption(map_data)
    write_blueprint(map_data, out_dir, spec=result["spec"], caption=caption)
    result["out_dir"] = str(out_dir)
    result["caption"] = caption
    return result


def generate(spec=None, map_data=None, seed=None, out_dir=None, cols=None, rows=None,
             timeout=2400, on_progress=None):
    """Full run: spec (or a ready map) -> layout -> Ideogram 4 -> image paths.

    Returns a dict with `images`, `map_json`, `caption` and `out_dir`.
    """
    cfg = load_config()
    comfy_cfg = cfg.get("comfy", {})

    if map_data is not None:
        map_data, problems = A.validate_map(map_data)
        result = {"map_json": map_data, "spec": None, "problems": problems}
    else:
        if spec is None:
            raise ValueError("pass either `spec` or `map_data`")
        result = build_map(spec, seed=seed, cols=cols, rows=rows)
        map_data = result["map_json"]

    out_dir = Path(out_dir) if out_dir else PROJECT / "output" / map_data["meta"]["name"]
    caption = make_caption(map_data)
    write_blueprint(map_data, out_dir, spec=result.get("spec"), caption=caption)

    # The planner is the other heavy tenant of the graphics card. Best-effort:
    # if Ollama is not running there is nothing to unload.
    try:
        ocfg = cfg.get("ollama", {}) or {}
        OllamaClient(base_url=ocfg.get("base_url", "http://127.0.0.1:11434"),
                     model=ocfg.get("model", "")).unload()
    except Exception:
        pass

    client = ComfyClient(comfy_cfg.get("base_url", "http://127.0.0.1:8188"))
    ok, detail = client.health()
    if not ok:
        raise RuntimeError(f"ComfyUI unreachable: {detail}")

    icfg = comfy_cfg.get("ideogram", {}) or {}
    width, height = target_size(map_data, icfg.get("target_megapixels", 1.8))
    graph = build_ideogram4(comfy_cfg, caption, seed=seed, width=width, height=height)

    prompt_id = client.queue_prompt(graph)
    outputs = client.wait(prompt_id, timeout=timeout, on_progress=on_progress)
    images = client.get_images(outputs, out_dir)
    for path in images:
        trim_to_margin(path, map_data)

    result.update({"images": images, "caption": caption, "out_dir": str(out_dir),
                   "prompt_id": prompt_id, "size": (width, height)})
    return result


# The old name for `blueprint`. It read as "only render", which is the opposite
# of what it does, so it misled every agent that met it first.
render_only = blueprint


def generate_from_map(map_dict_or_path, out_dir=None, **kwargs):
    """Render a map.json (or dict) that already exists."""
    if isinstance(map_dict_or_path, (str, Path)):
        map_dict_or_path = json.loads(Path(map_dict_or_path).read_text(encoding="utf-8"))
    return generate(map_data=map_dict_or_path, out_dir=out_dir, **kwargs)


def generate_map(scene_description, style_id=None, size="medium", out_dir=None,
                 seed=None, **kwargs):
    """End-to-end using the local LLM for planning."""
    cfg = load_config()
    oc = cfg.get("ollama", {})
    planner = MapPlanner(ollama_client=OllamaClient(
        base_url=oc.get("base_url", "http://127.0.0.1:11434"),
        model=oc.get("model", "qwen3.8:27b"),
        timeout=int(oc.get("timeout", 600))))
    plan = planner.plan_map(scene_description, style_id=style_id, size=size, seed=seed)
    return generate(map_data=plan["map_json"], out_dir=out_dir, seed=seed, **kwargs)
