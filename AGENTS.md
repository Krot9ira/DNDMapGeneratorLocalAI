# AI agent integration guide

You are already a capable language model, so you do not need the local Ollama step at all.
Write the design spec yourself and call the API — it is faster, frees the VRAM for the
renderer, and gives you direct control over the scene.

The tools live in `tools/`. They locate the project root themselves, so they work from the
repository or from an unpacked release.

---

## The one rule that matters

**Describe the place. Never compute coordinates.**

The architect (`tools/architect.py`) owns every spatial decision: room packing, corridor
routing, wall derivation, door placement, prop distribution. Your job is the semantic
layer — what kind of place this is, which areas it has, what it is made of, what is lying
around.

That split is why layouts are always valid. Earlier versions asked the model for rectangles
and got overlapping rooms, walls enclosing nothing, and doors in the middle of the floor.

The exception is deliberate annotation: once the geometry exists you *may* pin a rectangle
and describe exactly what belongs in it (see **Annotations** below).

---

## Quick start

```python
import sys; sys.path.insert(0, "tools")
import agent_api

result = agent_api.generate({
    "title": "Baldur's Gate Docks",
    "style": "city_harbour",
    "layout": "harbour",
    "grid": {"cols": 40, "rows": 30},
    "scene_summary": (
        "A stone-paved city dock along dark harbour water, one large wooden sailing ship "
        "moored alongside, a timber gangway from its deck up onto the quay."
    ),
    "render_details": (
        "Wet grey cobblestone with tar stains and puddles, heavy timber warehouse walls, "
        "tarred rope, iron mooring bollards, weathered oak deck planking"
    ),
    "prop_density": "high",
    "rooms": [
        {"label": "Cargo Warehouse",  "size": "l", "props": ["crate", "barrel", "cart"]},
        {"label": "Harbourmaster",    "size": "s", "props": ["table", "bookshelf"]},
        {"label": "Dockside Tavern",  "size": "m", "props": ["table", "hearth", "barrel"]},
    ],
})

print(result["images"])       # finished PNG paths
print(result["out_dir"])      # everything else lives here
```

---

## Spec fields

| Field | Required | Notes |
|---|---|---|
| `title` | yes | Human name of the place |
| `style` | yes | One of the ids from `agent_api.list_styles()` |
| `layout` | no | Defaults to the style's own layout |
| `grid` | no | `{"cols": N, "rows": N}`, 10–150 each. Or `size`: `small`/`medium`/`large`/`huge`/`giant` |
| `scene_summary` | yes | Two or three vivid sentences: surfaces, wall condition, what happened here |
| `render_details` | yes | Dense comma list of materials, finishes, colours, wear, damage, light |
| `rooms` | yes | 3–6 areas, each with `label`, `size` (`s`/`m`/`l`) and 5–9 `props` |
| `terrain` | no | `{"kind": "water\|pit\|rubble\|vegetation", "amount": "low\|medium\|high", "shape": "pools\|river"}` |
| `prop_density` | no | Defaults to `high` |
| `border` | no | Width of the blank bleed margin in cells, 0–8. Defaults to 2 |
| `seed` | no | Pass to `generate(seed=...)` for a reproducible layout |

### The bleed margin

Every finished map carries an empty ring of cells outside the field you asked for. **It is
added, not subtracted**: `{"cols": 25, "rows": 19}` gives you 25×19 cells to work with, and
the stored grid is 29×23 with the playable area offset by 2.

That matters when you read a `map.json` back or place anything by coordinate: **every
coordinate in the file is already offset by the margin.** `architect.playable_rect(map)`
returns `(x, y, w, h)` of the field, and `architect.border_of(map)` gives the width. Do not
subtract it yourself when annotating — annotations use the same coordinate space as the rest
of the file.

The margin exists because image models are least reliable at the very edge of a frame. Given
a blank rim, their guessing spoils empty space instead of the corner of a room. Pass
`"border": 0` if you genuinely want the map bled to the edge.

### Layouts

| Layout | Builds |
|---|---|
| `dungeon` | separate rooms joined by corridors — crypts, tombs, keeps |
| `building` | inside of one structure — tavern, shop, station |
| `cavern` | organic irregular cave |
| `open` | bare outdoor ground, no enclosing walls |
| `forest` | dense woodland; clearings are carved out of thicket |
| `swamp` | standing water, reed beds, islands of solid ground, plank walkways |
| `ruins` | open site strewn with fragments of collapsed building |
| `street` | city block with buildings along a road |
| `arena` | one dramatic chamber |
| `harbour` | quay, open water and a moored ship (the ship is generated for you) |
| `custom` | you supply explicit `x/y/w/h` per room and they are validated and walled |

---

## What the renderer is told exactly

Everything load-bearing is handed over as a bounding box, in this order of priority:

1. Your annotations and effects.
2. **Structure** — every wall run, every door, every window, the ship.
3. Rooms and terrain bodies.
4. Catalogue props with a described form.

Loose clutter is described without coordinates on purpose: pinning fifty small objects makes
the painting stiff and worse. If a specific object matters, give it as a custom prop with a
`label`, or mark its rectangle with an annotation.

---

## Pinning exact detail

Everything below carries its own bounding box into the caption and is marked as
load-bearing, so the renderer places it precisely. Use it when position matters.

### Annotations — any region, in your own words

```python
result = agent_api.build_map(spec, seed=7)
m = result["map_json"]
m["annotations"] = [
    {"label": "Portcullis", "elaboration": "some",
     "description": "A heavy iron portcullis lowered across the passage, spiked bars sunk "
                    "into slots in the flagstones",
     "x": 2, "y": 13, "w": 1, "h": 4},
]
agent_api.generate(map_data=m)
```

`elaboration` is `exact` (render only what I wrote), `some` (fill in fitting detail) or
`free` (elaborate richly).

### Custom props — a single object the catalogue has no word for

```python
m["features"].append({
    "kind": "custom", "x": 20, "y": 15, "structural": True,
    "label": "Iron winch drum",
    "description": "A tarred rope drum on an iron frame, chain running into a floor slot",
    "elaboration": "exact"})
```

### Effects — an atmospheric top layer

Effects never change the ground or block movement; they are light, smoke and weather
painted over the finished map.

```python
m["effects"] = [
    {"kind": "fog", "x": 4, "y": 4, "w": 8, "h": 6, "intensity": "high"},
    {"kind": "fire", "x": 20, "y": 12, "w": 3, "h": 3},
    {"kind": "custom", "label": "Soul wisps", "elaboration": "free",
     "description": "Pale blue drifting motes trailing thin light",
     "x": 14, "y": 6, "w": 5, "h": 5},
]
```

Built-in kinds: `fire`, `embers`, `smoke`, `fog`, `mist`, `fireflies`, `magic_glow`,
`holy_light`, `poison_gas`, `blood`, `ice`, `webs`, `sparks`, `ash`, `steam`, `shadow`.
`intensity` is `low` / `medium` / `high`.

---

## Writing a good spec

**Be concrete and commit to one value.** The caption format forbids hedging — no "various",
"such as", "or similar". Name the stone, name the timber, say what is chipped or damp.

**Props are objects only.** Furniture, containers, scenery, tools, light sources. Never
people, creatures or animals — the GM places those as tokens, and the renderer is explicitly
instructed to leave the map empty of figures.

**Props split into two classes automatically.** Load-bearing ones (pillars, altars, tables,
sarcophagi, braziers, masts) get pinned to exact cells. Small clutter (barrels, crates, rope,
bones) is only *described*, because forcing a blob at an exact cell produced anonymous lumps
while a free renderer paints convincing clutter on its own.

**Layout beats prose for structure.** If you need a harbour with a ship, set
`"layout": "harbour"` — do not merely mention a ship in the summary.

---

## Other entry points

```python
agent_api.list_styles()          # every style, keyed by id
agent_api.list_layouts()         # layouts and grid sizes
agent_api.render_only(spec)      # Stage 1 only — inspect the plan before spending GPU time
agent_api.generate_from_map(m)   # re-render an existing map.json or dict
agent_api.build_map(spec)        # spec -> map dict, no network at all
agent_api.make_caption(map_data) # the exact JSON caption that will be sent
```

`render_only` is the cheap iteration loop: it writes `preview.png` (a labelled blueprint you
can read) plus `map.json`, with no GPU work. Check the layout, then call `generate`.

A plan written this way opens directly in the desktop app: **File → Open map.json** lists
every plan under `output/` with a thumbnail, and restores the scene text, style, size,
layout and terrain it was built with.

---

## Command line equivalent

```bash
python tools/pipeline.py build spec.json --out output/my_map   # spec -> layout, no LLM
python tools/pipeline.py generate output/my_map/map.json
python tools/pipeline.py validate output/my_map/map.json
```

---

## Editing a map directly

`map.json` round-trips through the same code the editor uses, so you may edit it and
re-render. `zones` are rectangles painted in list order:

```json
{"id": "floor_1", "kind": "floor", "x": 1, "y": 1, "w": 19, "h": 13}
```

Kinds: `void`, `floor`, `wall`, `door`, `window`, `water`, `pit`, `rubble`, `vegetation`,
`stairs`, `bridge`. A `door` is only honoured where it sits between two wall cells —
elsewhere it is demoted to plain floor, because a door with open ground on both sides is an
opening, not a door.

Call `architect.validate_map()` afterwards — it clamps coordinates, repairs unknown kinds
and reports whether the map still has walkable space.

---

## Output files

| File | What it is |
|---|---|
| `battlemap*.png` | the finished map |
| `preview.png` | labelled blueprint — read this to check the layout |
| `map.json` | the layout, re-renderable, openable in the app |
| `caption.json` | the structured caption with bounding boxes sent to Ideogram 4 |
| `spec.json` | the spec that produced it |
