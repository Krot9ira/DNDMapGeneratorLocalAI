#!/usr/bin/env python3
"""
Stage 1 brain.

The language model is asked for a *design spec* - what kind of place this is,
which areas it has, what terrain and props belong there - and never for grid
coordinates. All geometry comes from `architect`, which cannot produce a broken
layout. This split is what fixed plans that used to come back with overlapping
rooms and doors in the middle of the floor.
"""
import json
from pathlib import Path

import architect as A
from ollama_client import OllamaClient

from paths import ROOT as PROJECT

# Where something goes, without asking a language model for coordinates. Each
# is a share of the playable field: (x, y, w, h) as fractions.
PLACEMENTS = {
    "centre": (0.34, 0.34, 0.32, 0.32),
    "north": (0.30, 0.06, 0.40, 0.24),
    "south": (0.30, 0.70, 0.40, 0.24),
    "west": (0.06, 0.30, 0.24, 0.40),
    "east": (0.70, 0.30, 0.24, 0.40),
    "north-west": (0.06, 0.06, 0.30, 0.30),
    "north-east": (0.64, 0.06, 0.30, 0.30),
    "south-west": (0.06, 0.64, 0.30, 0.30),
    "south-east": (0.64, 0.64, 0.30, 0.30),
    "along the north edge": (0.04, 0.02, 0.92, 0.13),
    "along the south edge": (0.04, 0.85, 0.92, 0.13),
    "along the west edge": (0.02, 0.04, 0.13, 0.92),
    "along the east edge": (0.85, 0.04, 0.13, 0.92),
    "across the middle": (0.04, 0.42, 0.92, 0.16),
    "down the middle": (0.42, 0.04, 0.16, 0.92),
    "over the whole map": (0.02, 0.02, 0.96, 0.96),
}
PLACES = sorted(PLACEMENTS)
_SIZE_SCALE = {"s": 0.62, "m": 1.0, "l": 1.35}


def place_in_field(where, size, cols, rows, border):
    """Turn "north-east" plus a size into a rectangle in field coordinates."""
    fx, fy, fw, fh = PLACEMENTS.get(str(where or "").lower(), PLACEMENTS["centre"])
    scale = _SIZE_SCALE.get(str(size or "m").lower(), 1.0)
    play_w, play_h = max(1, cols - 2 * border), max(1, rows - 2 * border)
    cx, cy = fx + fw / 2.0, fy + fh / 2.0
    fw, fh = min(0.96, fw * scale), min(0.96, fh * scale)
    fx, fy = cx - fw / 2.0, cy - fh / 2.0
    x = int(round(max(0.0, min(1.0 - fw, fx)) * play_w))
    y = int(round(max(0.0, min(1.0 - fh, fy)) * play_h))
    w = max(1, min(play_w - x, int(round(fw * play_w))))
    h = max(1, min(play_h - y, int(round(fh * play_h))))
    return x, y, w, h


SPEC_SCHEMA = {
    "type": "object",
    "properties": {
        "name": {"type": "string"},
        "title": {"type": "string"},
        "style": {"type": "string"},
        "layout": {"type": "string", "enum": A.LAYOUTS},
        "size": {"type": "string",
                 "enum": ["small", "medium", "large", "huge", "giant"]},
        "scene_summary": {"type": "string"},
        "render_details": {"type": "string"},
        "lighting": {"type": "string"},
        "prop_density": {"type": "string", "enum": ["low", "medium", "high"]},
        "terrain": {
            "type": "object",
            "properties": {
                "kind": {"type": "string",
                         "enum": ["none", "water", "pit", "rubble", "vegetation"]},
                "amount": {"type": "string", "enum": ["low", "medium", "high"]},
                "shape": {"type": "string", "enum": ["pools", "river"]},
            },
            "required": ["kind"],
        },
        "annotations": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "label": {"type": "string"},
                    "description": {"type": "string"},
                    "where": {"type": "string", "enum": PLACES},
                    "size": {"type": "string", "enum": ["s", "m", "l"]},
                },
                "required": ["label", "description", "where"],
            },
        },
        "effects": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "kind": {"type": "string", "enum": sorted(A.EFFECTS.keys())},
                    "where": {"type": "string", "enum": PLACES},
                    "size": {"type": "string", "enum": ["s", "m", "l"]},
                    "strength": {"type": "string", "enum": ["low", "medium", "high"]},
                },
                "required": ["kind", "where"],
            },
        },
        "terrain_zones": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "kind": {"type": "string",
                             "enum": ["water", "pit", "rubble", "vegetation", "bridge",
                                      "stairs", "wall", "floor"]},
                    "where": {"type": "string", "enum": PLACES},
                    "size": {"type": "string", "enum": ["s", "m", "l"]},
                },
                "required": ["kind", "where"],
            },
        },
        "rooms": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "label": {"type": "string"},
                    "description": {"type": "string"},
                    "size": {"type": "string", "enum": ["s", "m", "l"]},
                    "terrain": {"type": "string",
                                "enum": ["none", "water", "pit", "rubble", "vegetation"]},
                    "props": {"type": "array", "items": {"type": "string"}},
                    "enclosed": {"type": "boolean"},
                },
                "required": ["label", "description", "size", "props"],
            },
        },
    },
    "required": ["title", "layout", "size", "scene_summary", "render_details", "rooms"],
}

SYSTEM_PROMPT = """You are a professional tabletop RPG cartographer designing a top-down D&D battle map.

You describe scenes; you never compute coordinates. A separate deterministic engine
builds the actual geometry from your description, so do NOT output x/y/width/height.

The game master's description is often a single short line. Enriching it is your job:
invent plausible, specific, setting-consistent detail so the finished map feels lived in
rather than empty. Never answer with a bare restatement of the request.

Rules:
- Pick the `layout` that matches the scene:
  dungeon  = separate rooms linked by corridors (crypts, tombs, keeps, temples)
  building = the inside of one structure (tavern, house, shop, ship interior, station)
  cavern   = natural irregular cave
  open     = bare outdoor ground with no walls (field, crossroads, camp)
  forest   = dense woodland, clearings carved out of thicket
  swamp    = standing water with reed beds and islands of solid ground
  ruins    = open site strewn with fragments of collapsed building
  street   = city block with buildings along a road
  arena    = one single dramatic chamber (boss fight, throne room)
  harbour  = waterfront with a quay, a gangway and a moored ship
- `rooms` are the distinct areas of the scene, 3 to 6 of them. Each needs four things:
  * `label` - what this place is called, in two or three words. Name it for what happens
    there: "Smithy", "Flooded Vestry", "Cargo Hold", "Alchemist's Workroom". NEVER
    "Area 1", "Room 2", "Main Area", "Second Area" or any other numbered placeholder -
    a name that says nothing is worse than no name, because it is sent to the artist.
  * `description` - one sentence on what this particular room looks like and what it is
    for. Different for every room. This is drawn, so describe the floor, the state of it
    and what stands in it: "Soot-blackened stone floor, anvil in the middle, quench barrel
    steaming beside the forge."
  * `size` - s, m or l.
  * `props` - 5 to 9 objects that belong in it, varied between rooms so each has its own
    purpose. Two rooms with the same props are one room drawn twice.
- Props are physical objects only: furniture, containers, scenery, tools, light sources.
  NEVER list people, creatures, animals or monsters - the game master places those as
  tokens afterwards.
- Every building is drawn with its roof removed so the interior is visible from above.
  Never describe roofs, rooftops or tiles.
- `terrain` describes ground hazards covering the map.
- `scene_summary` is two or three vivid sentences describing what the place physically
  looks like from directly above: the surfaces underfoot, the state of the walls, what has
  happened here and what has been left behind.
- `render_details` is a dense comma-separated list of concrete materials, surface finishes,
  colours, wear and damage, stains, and the quality of the light. Be specific: name the kind
  of stone, the kind of timber, what is chipped, damp, scorched or overgrown.
- Work it out properly before you answer. Take the description apart: what is underfoot,
  what is built, what is growing, what is broken, what is burning, what blocks a line of
  sight, where a person comes in and where they can go. Every one of those is something
  you can say below, and anything you leave unsaid is invented by the artist instead.
- `annotations` are the strongest tool you have: a named region described in your own
  words, drawn exactly where you put it. Use one for every specific thing the description
  names - a fountain, a collapsed span, a stand of trees, a stair, an altar, a barricade.
  Give each a `label` of two or three words and a `description` of one or two sentences
  saying what it looks like from directly above. Place it with `where` and size it with
  `size`. Six to twelve of them is a rich map; none is a bare one.
- `terrain_zones` put the ground itself somewhere: water, pit, rubble, vegetation, bridge,
  stairs, wall or plain floor, placed with the same `where` and `size`. Use them whenever
  the description says a river runs somewhere, a cliff closes a side, a boardwalk crosses
  a bog or a floor has fallen in. Anything you call a cliff, a wall or a barricade in an
  annotation needs a `wall` zone under it, or the map will have open floor where you said
  there was rock.
- `effects` lay fire, smoke, fog, mist, embers, magic glow, sparks, ash, steam or shadow
  over a region. They sit on top of everything and change nothing underneath.
- `props` may be anything you can name, not only the catalogue: "bush of raspberries",
  "toppled milk churn", "cracked scrying bowl". An object nobody has a word for is still
  drawn, so name what the scene actually needs rather than the nearest stock item.
- `lighting` is optional and says how this place is lit and nothing else - the colour and
  quality of the light and where it falls off. Do not use it to say where anything stands:
  a lighting line that mentions a central fire puts one on the map whatever the plan says.
- `enclosed` on a room says whether it has walls round it. On an outdoor map a street, a
  square or a yard has none; a house standing on that street does.
Answer with JSON only."""


class MapPlanner:
    def __init__(self, ollama_client=None, styles_dir=None):
        self.client = ollama_client or OllamaClient()
        self.styles_dir = Path(styles_dir) if styles_dir else PROJECT / "styles"

    # -- style library ------------------------------------------------
    def load_base(self):
        path = self.styles_dir / "_base.json"
        if path.exists():
            try:
                return json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                pass
        return {"forbidden_suffix": "The scene is completely empty of people, creatures and "
                                    "animals, and carries no text, letters, numbers, labels or "
                                    "grid lines anywhere"}

    def load_styles(self):
        styles = {}
        if not self.styles_dir.exists():
            return styles
        for path in sorted(self.styles_dir.glob("*.json")):
            if path.stem.startswith("_"):
                continue
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError) as exc:
                print(f"[warn] cannot read style {path.name}: {exc}")
                continue
            styles[data.get("id", path.stem)] = data
        return styles

    def load_style(self, style_id):
        if not style_id:
            return None
        styles = self.load_styles()
        if style_id in styles:
            return styles[style_id]
        for style in styles.values():
            if style.get("name", "").lower() == str(style_id).lower():
                return style
        return None

    # -- planning -----------------------------------------------------
    def free_renderer(self):
        """Ask ComfyUI to drop its models before the planner needs the card.

        ComfyUI holds around 27 GB of weights once it has rendered. Silent and
        best-effort: if ComfyUI is not running there is nothing to free.
        """
        try:
            from comfy import ComfyClient
            cfg = json.loads((PROJECT / "config.json").read_text(encoding="utf-8"))
            url = (cfg.get("comfy") or {}).get("base_url", "http://127.0.0.1:8188")
            ComfyClient(url).free_memory()
        except Exception:
            pass

    def plan_spec(self, scene_description, style_id=None, size="medium",
                  sketch_path=None, temperature=0.75):
        """Ask the LLM for a design spec. Returns (spec, raw_text)."""
        self.free_renderer()
        styles = self.load_styles()
        schema = json.loads(json.dumps(SPEC_SCHEMA))
        if styles:
            schema["properties"]["style"] = {"type": "string", "enum": sorted(styles)}

        catalogue = "\n".join(
            f"- {sid}: {s.get('name', sid)} - {s.get('description', '')}"
            for sid, s in sorted(styles.items()))

        known = sorted(A.KNOWN_PROPS)
        mine = sorted(A.custom_props())

        parts = [f'Scene requested by the game master:\n"{scene_description}"\n']
        if style_id:
            style = styles.get(style_id, {})
            parts.append(f"Use the visual style `{style_id}` ({style.get('name', style_id)}).")
        else:
            parts.append("Choose the most fitting `style` from this library:\n" + catalogue)
        parts.append(f"Use size `{size}` unless the scene clearly needs another.")
        parts.append("Props the renderer has a concrete description for - prefer these "
                     "names: " + ", ".join(known))
        if mine:
            parts.append("Objects this game master defined themselves. Use them whenever the "
                         "scene calls for one - they are drawn exactly as described: "
                         + ", ".join(mine))
        parts.append("Anything else you name still works; it is simply left to the artist's "
                     "judgement rather than drawn to a fixed description.")
        if sketch_path:
            parts.append("A sketch of the intended layout is attached. Match its areas, "
                         "their rough arrangement and any water or obstacles you can see.")
        parts.append("Return the design spec as JSON now.")

        # Time spent here is nothing beside the render that follows, so the
        # model is given room to think and room to answer. If the reasoning
        # pass tangles the JSON - some models put their working in the answer -
        # it is asked again plainly rather than failing.
        def ask(reasoning):
            return self.client.generate(
                "\n".join(parts),
                system=SYSTEM_PROMPT,
                format=schema,
                temperature=temperature,
                think=reasoning,
                images=[sketch_path] if sketch_path else None,
                num_predict=4096,
                num_ctx=16384,
            )

        raw = ask(True)
        spec = self.client.extract_json(raw)
        if not isinstance(spec, dict):
            raw = ask(False)
            spec = self.client.extract_json(raw)
        if not isinstance(spec, dict):
            raise ValueError(f"Planner returned no usable JSON.\n{raw[:600]}")
        spec.setdefault("size", size)
        if style_id:
            spec["style"] = style_id
        return spec, raw

    def plan_map(self, scene_description, style_id=None, size="medium", seed=None,
                 sketch_path=None, cols=None, rows=None):
        """Full Stage 1: description -> spec -> validated geometry."""
        spec, raw = self.plan_spec(scene_description, style_id=style_id, size=size,
                                   sketch_path=sketch_path)
        return self.compose(spec, seed=seed, cols=cols, rows=rows,
                            requested_style=style_id, raw=raw)

    def compose(self, spec, seed=None, cols=None, rows=None, requested_style=None, raw=""):
        """Turn a spec (from the LLM, an agent or a preset) into a finished map."""
        spec = dict(spec)
        if requested_style:
            spec["style"] = requested_style
        style = self.load_style(spec.get("style"))
        if style:
            spec.setdefault("style_props", style.get("props") or [])
            if not spec.get("layout"):
                spec["layout"] = style.get("default_layout", "dungeon")
        if cols and rows:
            spec["grid"] = {"cols": int(cols), "rows": int(rows)}

        # Terrain the planner placed itself, turned into rectangles here. It
        # names a direction; the arithmetic is not its job.
        grid_cfg = spec.get("grid") or {}
        plan_cols = int(grid_cfg.get("cols") or 0)
        plan_rows = int(grid_cfg.get("rows") or 0)
        if not plan_cols or not plan_rows:
            plan_cols, plan_rows = A.SIZE_PRESETS.get(str(spec.get("size", "medium")),
                                                      A.SIZE_PRESETS["medium"])
        zones = []
        for zone in (spec.get("terrain_zones") or []):
            if isinstance(zone, dict) and "where" in zone:
                zx, zy, zw, zh = place_in_field(zone.get("where"), zone.get("size"),
                                                plan_cols, plan_rows, 0)
                zones.append({"kind": zone.get("kind", "rubble"),
                              "x": zx, "y": zy, "w": zw, "h": zh})
            elif isinstance(zone, dict):
                zones.append(zone)
        if zones:
            spec["terrain_zones"] = zones

        map_data = A.build(spec, seed=seed)
        # `render_details` belongs to the map, not the spec: the caption builder
        # reads it from meta when it assembles the style block.
        if spec.get("render_details"):
            map_data["meta"]["render_details"] = spec["render_details"]
        if spec.get("lighting"):
            map_data["meta"]["lighting"] = spec["lighting"]

        # Regions the planner described in its own words, and the atmosphere it
        # laid over them. Both were things only a Python agent could reach; a
        # language model can now ask for them by naming a direction.
        border = A.border_of(map_data)
        cols = int(map_data["grid"]["cols"]) - 2 * border
        rows = int(map_data["grid"]["rows"]) - 2 * border
        for note in (spec.get("annotations") or []):
            if not isinstance(note, dict) or not str(note.get("label", "")).strip():
                continue
            nx, ny, nw, nh = place_in_field(note.get("where"), note.get("size"),
                                            cols, rows, 0)
            map_data.setdefault("annotations", []).append({
                "label": str(note["label"]).strip(),
                "description": str(note.get("description", "")).strip(),
                "elaboration": note.get("elaboration", "exact"),
                "x": nx + border, "y": ny + border, "w": nw, "h": nh,
            })
        for fx in (spec.get("effects") or []):
            if not isinstance(fx, dict) or not str(fx.get("kind", "")).strip():
                continue
            ex, ey, ew, eh = place_in_field(fx.get("where"), fx.get("size"), cols, rows, 0)
            map_data.setdefault("effects", []).append({
                "kind": str(fx["kind"]).strip().lower(),
                "intensity": fx.get("strength", "medium"),
                "x": ex + border, "y": ey + border, "w": ew, "h": eh,
            })
        map_data, _repairs = A.validate_map(map_data)
        return {
            "spec": spec,
            "map_json": map_data,
            "style": style,
            "raw": raw,
        }


if __name__ == "__main__":
    import sys

    desc = sys.argv[1] if len(sys.argv) > 1 else "A flooded gothic crypt with sarcophagi"
    planner = MapPlanner()
    result = planner.plan_map(desc, size="medium", seed=1)
    print(json.dumps(result["spec"], indent=2, ensure_ascii=False))
    print("Areas:", ", ".join(a["label"] for a in result["map_json"]["areas"]))
