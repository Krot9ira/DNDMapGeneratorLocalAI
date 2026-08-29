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
import re
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
        "new_style": {
            "type": "object",
            "properties": {
                "id": {"type": "string"},
                "name": {"type": "string"},
                "category": {"type": "string"},
                "description": {"type": "string"},
                "ground": {"type": "string"},
                "materials": {"type": "string"},
                "lighting": {"type": "string"},
                "boundary": {"type": "string"},
                "enclosure": {"type": "string",
                              "enum": ["masonry", "rock", "timber", "open"]},
                "palette": {"type": "string"},
                "hex_palette": {"type": "array", "items": {"type": "string"}},
                "props": {"type": "array", "items": {"type": "string"}},
                "tags": {"type": "array", "items": {"type": "string"}},
            },
            "required": ["id", "name", "ground", "materials", "lighting", "enclosure"],
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
- `style` must be one of the styles in the library below. If not one of them is the kind
  of place the game master asked for - the library has no snowbound village, no circus, no
  mushroom farm - keep `style` as the nearest one and ALSO fill `new_style`, and the new
  style is written into the library and used instead. Do this when the kind of place is
  wrong, not when the mood is: a night scene in a style written for daylight only needs
  `lighting`. A new style needs, and is judged on, five things:
  * `ground` - what is underfoot across the whole map, in a few words. It opens the
    background text of the caption, so it is the single strongest thing you write.
  * `materials` - what this place is made of. Open with one short sentence naming the kind
    of place ("A frozen fishing village seen from directly above."), because when the map
    has things of its own only that first sentence is kept; put the rest after it.
  * `lighting` - the colour and quality of the light and nothing else. Never where
    anything stands.
  * `enclosure` - what closes the site in: `masonry` walls, `rock` cliffs, a `timber`
    hull, or `open` ground with no edge at all. It decides what every boundary in the
    picture is made of.
  * `boundary` - what that edge looks like from above, in one phrase.
  Everything in a style is seen from directly above: nothing on a wall, nothing hanging,
  no ceiling, no roof and no side of anything.
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

    # -- authoring a style --------------------------------------------
    def install_style(self, data, layout=None, overwrite=False, force=False,
                      origin="agent"):
        """Write a new style into the library and return what was installed.

        A style is the strongest text in the caption: `ground` opens the
        background, `materials` says what kind of place this is, `lighting` is
        handed over whole and `enclosure` decides what the edge of the site is
        made of. Painting a scene with the nearest wrong style therefore paints
        the wrong map - a merchant caravan on a night meadow came out of
        `bandit_camp` as a palisaded camp on churned mud, and no wording
        anywhere else in the caption could beat it. So when nothing in the
        library is the kind of place that was asked for, the answer is to write
        the style, not to bend the scene onto one that does not fit.

        Both the local planner and an agent come through here, so a style is
        checked once, in one place, before it can reach a render.
        """
        from ideogram_prompt import style_problems   # circular at module level

        if not isinstance(data, dict):
            raise ValueError("a style must be an object")
        style = {k: v for k, v in data.items() if v not in (None, "", [], {})}
        sid = str(style.get("id") or style.get("name") or "").strip().lower()
        sid = re.sub(r"[^a-z0-9]+", "_", sid).strip("_")
        if not sid:
            raise ValueError("a style needs an `id` or a `name` to be filed under")
        style["id"] = sid
        style.setdefault("name", sid.replace("_", " ").title())
        if layout and not style.get("default_layout"):
            style["default_layout"] = layout
        # Everything the caption falls back to anyway is written out in full, so
        # the file reads like the shipped ones and the app needs no fallback.
        base = self.load_base()
        style.setdefault("aesthetics", base.get("aesthetics", ""))
        style.setdefault("hex_palette", list(base.get("default_palette") or []))
        style.setdefault("props", [])
        style.setdefault("tags", [])
        # Where a style came from, so the app can say so. The library that
        # ships with the program is "shipped"; a file somebody wrote by hand is
        # "user"; this is the third case, and a person choosing a style is
        # entitled to know which of the three they are looking at.
        style.setdefault("origin", origin)
        # What closes the site in is the field with the longest reach and the
        # one an author is most likely to leave out, so it is resolved here,
        # once, rather than guessed from the category on every render.
        style.setdefault("enclosure", A.enclosure_of(style, style.get("default_layout")))

        problems = style_problems(style)
        if problems and not force:
            raise ValueError("this style would paint a bad map:\n  - "
                             + "\n  - ".join(problems))

        installed = self.load_styles()
        path = self.styles_dir / f"{sid}.json"
        if (sid in installed or path.exists()) and not overwrite:
            raise ValueError(f"style '{sid}' already exists; pass overwrite=True to "
                             f"replace it, or choose another id")
        self.styles_dir.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(style, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8")
        return {"id": sid, "path": str(path), "style": style, "problems": problems}

    def install_spec_style(self, spec):
        """Install `new_style` if a spec carries one, and point the spec at it.

        The planner's schema keeps `style` an enum of what is installed, so a
        model can never name a style that does not exist; when it wants one that
        does not, it writes the style itself in `new_style` and this puts it in
        the library before anything reads it back.
        """
        data = spec.pop("new_style", None)
        if not isinstance(data, dict) or not data:
            return None
        try:
            made = self.install_style(data, layout=spec.get("layout"),
                                      overwrite=bool(data.get("overwrite")),
                                      origin="agent")
        except ValueError as exc:
            print(f"[warn] new style not installed: {exc}")
            return None
        spec["style"] = made["id"]
        return made

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
            parts.append("If none of them is the kind of place this scene is - not merely "
                         "the wrong weather or the wrong time of day - keep `style` as the "
                         "nearest one and write the style this scene needs in `new_style`.")
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
        # A style the game master picked by hand outranks anything the model
        # would rather have painted with.
        if style_id:
            spec["style"] = style_id
            spec.pop("new_style", None)
        else:
            self.install_spec_style(spec)
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
        # A spec may carry its own seed, as AGENTS.md promises it can. An
        # explicit argument outranks it; without either the layout randomises
        # and meta.seed used to record None.
        if seed is None and spec.get("seed") is not None:
            try:
                seed = int(spec["seed"])
            except (TypeError, ValueError):
                pass
        if requested_style:
            spec["style"] = requested_style
            spec.pop("new_style", None)
        # A spec that brings its own style with it - because nothing installed
        # was the kind of place it needed - installs it here, so every entry
        # point into the pipeline supports one and not only the local planner.
        self.install_spec_style(spec)
        style = self.load_style(spec.get("style"))
        if style:
            spec.setdefault("style_props", style.get("props") or [])
            if not spec.get("layout"):
                spec["layout"] = style.get("default_layout", "dungeon")
        if cols and rows:
            spec["grid"] = {"cols": int(cols), "rows": int(rows)}

        # A planner names a direction; an agent or a scene file gives a
        # rectangle outright. Both end up in the spec as rectangles in field
        # coordinates, and the architect places them, so the bleed margin shifts
        # them along with everything else on the map.
        grid_cfg = spec.get("grid") or {}
        plan_cols = int(grid_cfg.get("cols") or 0)
        plan_rows = int(grid_cfg.get("rows") or 0)
        if not plan_cols or not plan_rows:
            plan_cols, plan_rows = A.SIZE_PRESETS.get(str(spec.get("size", "medium")),
                                                      A.SIZE_PRESETS["medium"])

        def placed(item, extra=None):
            out = dict(extra or {})
            if "where" in item and "x" not in item:
                x, y, w, h = place_in_field(item.get("where"), item.get("size"),
                                            plan_cols, plan_rows, 0)
            else:
                x, y = int(item.get("x", 0)), int(item.get("y", 0))
                w, h = max(1, int(item.get("w", 1))), max(1, int(item.get("h", 1)))
            out.update({"x": x, "y": y, "w": w, "h": h})
            return out

        zones = []
        for zone in (spec.get("terrain_zones") or []):
            if isinstance(zone, dict) and zone.get("kind"):
                zones.append(placed(zone, {"kind": zone["kind"]}))
        spec["terrain_zones"] = zones

        notes = []
        for note in (spec.get("annotations") or []):
            if isinstance(note, dict) and str(note.get("label", "")).strip():
                notes.append(placed(note, {
                    "label": str(note["label"]).strip(),
                    "description": str(note.get("description", "")).strip(),
                    "elaboration": note.get("elaboration", "exact")}))
        spec["annotations"] = notes

        layers = []
        for fx in (spec.get("effects") or []):
            if isinstance(fx, dict) and str(fx.get("kind", "")).strip():
                layers.append(placed(fx, {
                    "kind": str(fx["kind"]).strip().lower(),
                    "intensity": fx.get("intensity", fx.get("strength", "medium"))}))
        spec["effects"] = layers

        map_data = A.build(spec, seed=seed)
        # `render_details` belongs to the map, not the spec: the caption builder
        # reads it from meta when it assembles the style block.
        if spec.get("render_details"):
            map_data["meta"]["render_details"] = spec["render_details"]
        if spec.get("lighting"):
            map_data["meta"]["lighting"] = spec["lighting"]
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
