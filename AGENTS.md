# AI agent integration guide

This folder generates D&D battle maps. You drive it from Python.

---

## Start here

**1. Where to run.** From the folder that contains `tools/`, `styles/` and `config.json` -
the folder this file is in. Everything resolves relative to it.

**2. How to import.**

```python
import sys; sys.path.insert(0, "tools")
import agent_api
```

**3. Which call.** These are not interchangeable. Pick from the request, not from habit.

| You were asked for | Call | Takes | Produces |
|---|---|---|---|
| a **plan**, layout, blueprint, schema, floor plan | `agent_api.blueprint(spec)` | seconds, no GPU | `map.json` + readable `preview.png` |
| a **finished map**, a render, an image to play on | `agent_api.generate(spec)` | minutes, needs ComfyUI | painted `battlemap*.png` |
| several options to choose between | `blueprint` per option | seconds each | one folder each |
| a render of a plan that exists already | `agent_api.generate_from_map(path_or_dict)` | minutes | painted image |

> **If the request is ambiguous, call `blueprint`.** It is cheap and reversible, and its
> output can be rendered later without redoing the planning. Four renders when somebody
> asked for four layouts is the expensive mistake; the reverse costs nothing.

**4. The one rule.** Describe the place. **Never compute coordinates.** Room positions,
corridors, walls, doors and prop placement are all worked out for you by
`tools/architect.py`. Your job is what kind of place it is, which areas it has, what it is
made of, what is lying about. Earlier versions asked a model for rectangles and got
overlapping rooms and doors in the middle of the floor; this split is why a broken layout
is now impossible to express.

The one exception is deliberate: once the geometry exists you may pin a rectangle and say
exactly what belongs in it. See **Annotations**.

---

## The smallest thing that works

```python
import sys; sys.path.insert(0, "tools")
import agent_api

plan = agent_api.blueprint({
    "title": "Riverside Hamlet",
    "style": "village_hamlet",
    "grid": {"cols": 30, "rows": 24},
    "scene_summary": "Thatched cottages either side of a rutted dirt lane, a stone well.",
    "rooms": [{"label": "Smithy", "size": "m", "props": ["anvil", "forge", "barrel"]}],
})

print(plan["out_dir"])     # map.json and preview.png are written here
```

Three fields are required: `title`, `style`, `scene_summary`. Everything else has a
sensible default. `agent_api.list_styles()` returns every style id.

### What you get back

Both calls return a dict:

| Key | What it is |
|---|---|
| `out_dir` | the folder everything was written to |
| `map_json` | the plan as a dict - areas, walls, props, the lot |
| `caption` | exactly what the painter is told, if you want to inspect it |
| `images` | list of finished PNG paths. **`blueprint` does not set this** |
| `problems` | anything the validator quietly repaired |

### Did it work?

- `blueprint`: `out_dir` contains `map.json` and `preview.png`. Open the preview to see
  the layout. Report the folder and describe what came out.
- `generate`: `result["images"]` is a non-empty list of files that exist on disk. If it is
  empty, the render failed - say so, do not claim success.

---

## A full spec, with everything worth setting

```python
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

print(result["images"])
```

---

## Render settings

Everything comes from `config.json`, shared with the app, so an agent renders exactly as a
person does: **Quality, 48 steps**, guidance 7 dropping to 3 over the last third of the
schedule, euler, 1.8 megapixels (a 34x26 map lands at 1504x1200), random seed.

Override any of it for one call without touching the file:

```python
agent_api.generate(spec, comfy_overrides={"preset": "Ultra", "target_megapixels": 2.4})
```

Presets are `Turbo` 12 steps, `Default` 20, `Quality` 48, `Ultra` 64. Past Quality the
sampler is well into diminishing returns.

## Seeing what would be sent

```bash
python tools/pipeline.py caption output/my_map/map.json
```

The app can do the same without opening a window, which is what to ask for when somebody
reports a bad render:

```bash
DndBattlemapGenerator.exe --caption path	o\map.json out.json
```

`tools/check_caption_parity.py` compares the two across every layout. They should be byte
for byte identical; if they are not, the app and the tools will paint the same plan
differently.

## When it goes wrong

| What you see | What it means |
|---|---|
| `ModuleNotFoundError: agent_api` | you are in the wrong folder, or forgot `sys.path.insert(0, "tools")` |
| `RuntimeError: ComfyUI unreachable` | ComfyUI is not running. `blueprint` still works; `generate` cannot |
| `generate` returns no images | the render failed. The reason is in the exception text - report it |
| `ModuleNotFoundError: PIL` | run `pip install pillow`; the preview needs it |
| the style id is rejected | call `agent_api.list_styles()` and use one of those keys - or write the style yourself, below |
| a render takes many minutes | that is normal. Quality is 48 steps. Do not retry over the top of it |

Never say a map was produced without checking that the file exists.

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
| `rooms` | yes | 3–6 areas, each with `label`, `description`, `size` (`s`/`m`/`l`) and 5–9 `props` |
| `terrain` | no | `{"kind": "water\|pit\|rubble\|vegetation", "amount": "low\|medium\|high", "shape": "pools\|river"}` |
| `prop_density` | no | Defaults to `high` |
| `border` | no | Width of the blank bleed margin in cells, 0–8. Defaults to 2 |
| `seed` | no | Pass to `generate(seed=...)` for a reproducible layout |

### Naming the rooms

Each room's `label` and `description` are sent to the renderer with that room's rectangle,
so they decide what gets painted there. Both are worth writing properly.

```python
{"label": "Smithy",
 "description": "Soot-blackened stone floor, anvil in the middle, quench barrel steaming "
                "beside the forge.",
 "size": "m",
 "props": ["anvil", "forge", "barrel", "workbench", "crate"]}
```

- **Name it for what happens there.** "Smithy", "Flooded Vestry", "Cargo Hold". Never
  "Area 1", "Room 2" or "Main Area" - a name that says nothing is worse than no name,
  because it is handed to the artist as if it meant something. `validate` reports these.
- **A different description for every room.** One sentence on the floor, its state, and
  what stands in it. Two rooms with the same description are one room drawn twice.
- **Vary the props.** They are what makes one area read differently from the next.

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
| `cavern` | chambers joined by winding passages, no straight lines |
| `open` | bare outdoor ground, no enclosing walls |
| `forest` | dense woodland; clearings are carved out of thicket |
| `swamp` | standing water, reed beds, islands of solid ground, plank walkways |
| `ruins` | open site strewn with fragments of collapsed building |
| `street` | city block with buildings along a road, open ground beyond |
| `district` | wall-to-wall city: buildings edge to edge with alleys between |
| `arena` | a sand fighting floor inside a ringed barrier with gates |
| `harbour` | quay, open water and a moored ship (the ship is generated for you) |
| `deck` | one ship under way, its deck filling the map, open water all round |
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

---

## How the renderer reads what you give it

Hard-won, all of it from renders that came back wrong. These are the rules the caption
builder now follows on your behalf, and the ones you still have to follow yourself.

### Every rectangle you hand over is something to draw

There is no such thing as a rectangle meaning "nothing here". Ask for a bounding box round
an empty floor and you get a room built on its outline - that is where an inner wall came
from in a hall that was supposed to be one open space.

- **Do** put a rectangle on a thing that exists in one place: a fountain, a staircase, a
  gate, a specific door.
- **Do not** put one round an absence, a mood, or a region you simply want left alone. Say
  that in the room's own `description`, where there is no outline to trace.

### Four rectangles round four walls make a fifth wall

Annotations hugging the left, right, top and bottom of a room read as a frame, and a frame
gets joined up into a wall. Furniture that stands against a wall belongs in the room's
`description`, worded as touching the wall along its whole length.

### Silence gets filled, always

Whatever you leave undescribed, the renderer invents something for. Take the perimeter
annotations away without replacing them and the same band comes back chopped into cubicles.
Open ground is described for you now, as real blocks, for exactly this reason.

### One room has to be stated, not implied

A large building with a single area used to be handed over as a footprint alone, and the
renderer subdivided it - a grand hall came back as a hotel corridor. When a building holds
exactly one area the caption now also says, with that area's rectangle, that it is one
undivided space. You get this automatically; it is worth knowing why it is there.

### The style's own words are part of the instruction

A style file's `materials` text goes into the caption background, which outweighs any single
element. If it describes something the plan does not have, the style wins. Six render rounds
went into arguing with `city_townhouse`, whose text says "partition walls divide the inside
into rooms" - correct for a townhouse, fatal for a single hall, and no amount of saying
"one undivided room" elsewhere in the caption could beat it.

`build_map` now warns when a style written for divided interiors is used on a map that is
one room filling the field. Read `result["problems"]` before spending GPU time on it.

### A sentence that describes the map is a sentence that commands it

The caption used to finish with "the buildings are of different sizes and stand in an
irregular arrangement" on every map ever built, including maps with one room in them. It
was written as a caveat against symmetry. It reads as an instruction to draw several
buildings, and it is the last sentence in the description, which is the strongest place in
the whole caption. A tavern that is one hall came back ringed with a dozen little timber
bays round its walls, over and over, and nothing said anywhere else about "one undivided
space" could beat it.

Nothing in the caption may now describe something the plan does not contain. The
arrangement sentence is written after the elements are built, when the number of buildings
is actually known, and says one, several or nothing accordingly.

### A gorge is not a building

Every map used to be described as though it were a walled house: the ring of wall round the
playing field became "one single large building with its roof removed", its cliffs became
"one continuous face of plain masonry", and the way in became "a plain timber panel with
dark iron bands". A goblin camp in a rock gorge is not any of those things.

What closes a site in is now a property of the site. A style may state it outright with
`"enclosure": "masonry" | "rock" | "timber" | "open"`; otherwise the layout decides, and for
`custom` - which is what every agent-written scene uses - the style's category does. It
changes four things at once, in the app and the tools alike:

- what the boundary is called (`"boundary"` in the style file, or a neutral default),
- what its face is made of, everywhere the caption mentions a wall,
- whether a wall ring filling the field may be called a building at all,
- and whether a sealed area gets a door cut into it or a passage carried off the edge of
  the map. Caves do not have doors in them, and you walk into a clearing rather than
  knocking.

If your scene has already pinned what the edge is - cliffs down both sides, a treeline
along the top - the generic boundary sentence is dropped, because two answers to one
question is worse than none.

### Say a thing once, at one rectangle

A building and the room inside it are two true rectangles, the second inset by the
thickness of the wall. Handed over as two elements they leave a ring between them, and a
ring is a place, so the renderer furnishes it. A building holding exactly one room is now
described once, at the outer rectangle, and the same rule kills the duplicate "open ground"
element that used to sit under every named area.

### Negations are weak, positive statements are strong

"No arch, no gap" is close to useless: the text encoder handles negation badly. State what
the thing is instead - "one continuous face of plain masonry from corner to corner" - and it
holds. Counts attached to the thing itself ("one doorway breaks this building's wall") hold
far better than a count stated once for the whole map.

### Word a thing by what it looks like from above

"A door with a ring handle" is a front view, and asking for one gets you a door drawn as if
you were standing in front of it, lying flat on the map. Describe the top face: what you
would see looking straight down. The same trap catches ceilings - mention a ceiling, a
chandelier or cornice moulding and the whole map tips into perspective, because a ceiling
can only be shown from the side. The shared contract now forbids all of that outright.

### Nothing is on a wall and nothing hangs

A wall is a vertical surface. If something is on one, the only way to show it is
from the side, so the renderer turns the map on its edge to fit it in and the
whole picture comes back in perspective. Three cave renders in a row were drawn
with the top of the map as a wall face in elevation, alcoves and all, because the
style said its crystals grew "from the walls" - four separate sentences saying
the view is straight down could not undo one phrase that needed a side view.

Eleven of the shipped styles were doing some version of it: wall-mounted torches,
hanging lanterns and banners, lamp brackets on the walls, floor-to-ceiling
bookcases, barrel-vaulted tunnels, gratings overhead. All of them now say where
the thing stands on the floor. `style_warnings` flags anything on a wall,
hanging, suspended or under a ceiling, and `check_captions.py` fails on it, so a
style written later cannot quietly bring it back. "Against the walls" and "along
the walls" are fine - those things are standing on the ground.

### Say what a thing is, not what it is not - including about the whole picture

Every caption used to end with "the layout is not symmetrical, not mirrored and
not a repeating pattern", and two cave renders came back with their left half a
perfect mirror of their right. The word being acted on there is "mirrored". It
now says the left half and the right half are different from each other, that so
are the top and the bottom, and that each thing in the picture appears once, in
one place, at its own angle.

The same trap, one level down: "thick unbroken outer walls right on the edges of
the rectangle" is an invitation, and a wall one square deep out of thirty came
back as a band a sixth of the map wide - and a band that wide is a place, so it
got furnished with alcoves. The building element now works the thickness out
from the plan and says it: a narrow line about three percent of the width of this
rectangle and no wider, a line, not a band.

### Small boxes round the inside of a room become compartments

Twelve of a tavern's twenty-four elements were filler props, each pinned with its
own rectangle, and almost all of them stood against a wall. A ring of small boxes
inside a room reads as a row of compartments, and the renderer drew one - a dozen
little timber bays, through four rounds of trying to talk it out of them. Filler
standing against a wall is no longer pinned; it goes into the clutter sentence,
which says what there is without saying where each piece stands. Filler is also
never pinned on top of something the scene has already described, which is how
five heaps of rubble ended up inside "a round stone well head standing alone".

Things you asked for by name keep their rectangles. This only applies to what the
generator added on its own.

### A prop's own name can carry the wall into the caption

Left to plan a room, a language model invents prop kinds with the mounting baked
into the name: "faint_chalk_mark_on_the_wall", "rusted_iron_sword_on_a_nail".
Spelled out as a phrase, those say exactly what no caption may say, and the old
lint never looked at prop names. Every kind is now reworded or cut down to
floor-safe words before it reaches the renderer - the chalk mark keeps its chalk
and loses its wall - identically in the tools and the app. What cannot be
rewritten is scene prose asking for the side view itself: a cauldron that "hangs
over the flames" is warned about when the plan is built, so the wording can be
fixed where it was written.

### The edge of the site is not the walls inside it

What closes a site in decides what its boundary is made of and nothing else. A
stone house on a burning street is built of masonry even though the street it
stands on is open air, and for a while it was being told its walls were raw rock.
Whether one area is a room is likewise asked of that area - is a reasonable part
of its outline wall of its own? - and not of the map it sits on, with the
boundary itself not counting, or every cave would be a room: the architect rings
the whole playing field whatever is on it.

### Atmosphere goes in the background, not in a rectangle

An effect covering most of the map came back as six identical elements saying the
same sentence over six boxes: a quarter of the element budget, spent on exactly
the repetition the rest of the caption argues against. Anything covering a
quarter of the map or more is atmosphere and goes into the background text, where
it costs no slots and cannot repeat itself. Smaller effects are still tiled so
the renderer covers them properly, four tiles at most.

### The caption has a budget, and spending it costs adherence

Past roughly forty elements everything degrades at once: the layout comes back mirrored,
interiors are subdivided past the plan, and the ban on people can fail. Twenty-odd strong
elements beat forty-five weak ones. The builder caps it and spends the budget in priority
order, so the things you pinned deliberately go first.

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

## When no style fits, write one

The style is not decoration. Four of its fields outrank most of what you write in the
spec:

| Field | Where it lands |
|---|---|
| `ground` | the first words of the caption background - what is underfoot everywhere |
| `materials` | what kind of place this is. Only the **first sentence** survives when your map has annotations of its own; write that sentence to name the place |
| `lighting` | handed to the renderer whole, unless the spec sets its own `lighting` |
| `enclosure` | `masonry` / `rock` / `timber` / `open` - decides what every boundary in the picture is made of, and whether a sealed area gets a door or a way off the edge of the map |

So painting a scene with the nearest *wrong* style paints the wrong map, and nothing you
write elsewhere can argue it back. A peaceful merchant caravan on a night meadow was tried
against every installed style: `bandit_camp` grounds the map in "trampled bare earth
churned to mud" and describes a palisade, `farmstead` in ploughed furrows, `lush_forest`
in a forest clearing. None of them is a meadow.

**When the library has no style for the kind of place you were asked for, write one.** It
is a normal move, not a last resort - the same thing a person does by hand. It is the
wrong move for the wrong *mood*: a night scene in a style written for daylight only needs
the spec's own `lighting`.

```python
agent_api.create_style({
    "id": "caravan_camp",                    # lower case, digits, underscores
    "name": "Caravan Camp",
    "category": "Wilderness",
    "description": "Merchant wagons drawn up in a ring on open grassland.",
    "default_layout": "open",
    "enclosure": "open",
    "ground": "soft green meadow grass, worn to bare dark earth where the camp is trodden",
    "materials": "A merchant caravan camped for the night on open grassland, seen from "
                 "directly above. Heavy timber wagons with arched grey canvas covers, "
                 "stone-ringed campfires, rope lines strung between the wagons ...",
    "lighting": "deep blue night lit by warm orange firelight, long soft shadows",
    "boundary": "unbroken open grassland running flat in every direction",
    "props": ["wagon", "campfire", "crate", "barrel"],
    "hex_palette": ["#3E5A38", "#6E7F52", "#C4712B", "#7A6247", "#1E2733"],
})
```

`id`, `name`, `ground`, `materials`, `lighting` and `default_layout` are required;
everything else is filled from the shared base. Then use it like any other:
`{"style": "caravan_camp", ...}`.

The style is checked before it is written, and refused if it would paint a bad map:

- a `lighting` line that places something ("a fire in the middle") - it would put that
  thing on every map the style ever paints,
- any wording that can only be seen from the side, in any field: on a wall, hanging,
  overhead, a ceiling, a roof, a facade. One such phrase tips the whole picture into
  perspective,
- a `default_layout` or `enclosure` that does not exist, a bad `#RRGGBB`, an id that is
  not a slug, an id already taken (`overwrite=True` replaces it).

`force=True` writes it anyway and hands the problems back in `problems`. Run
`python tools/check_captions.py` afterwards: it builds a caption for every installed style
and will fail on anything the style breaks map-wide.

Styles record where they came from. `create_style` stamps `"origin": "agent"`, the shipped
library says `"shipped"`, and a file somebody wrote by hand says `"user"` or nothing at
all. The desktop app shows an **AI** or **YOURS** badge on those cards, so a person can
always tell which styles they are looking at.

The local planner can do this too: its schema has a `new_style` object beside `style`, and
anything it writes there is validated, installed and used for that map.

---

## Other entry points

```python
agent_api.list_styles()          # every style, keyed by id
agent_api.create_style(style)    # write a new style when none of them fits
agent_api.list_layouts()         # layouts and grid sizes
agent_api.blueprint(spec)        # Stage 1 only — the plan, in seconds, with no GPU
agent_api.generate_from_map(m)   # re-render an existing map.json or dict
agent_api.build_map(spec)        # spec -> map dict, no network at all
agent_api.make_caption(map_data) # the exact JSON caption that will be sent
```

`blueprint` is the cheap iteration loop: it writes `preview.png` (a labelled blueprint you
can read) plus `map.json`, with no GPU work. Check the layout, then call `generate`.
(`render_only` is the old name for it and still works, but it read as "only render", which is
the opposite of what it does.)

A plan written this way opens directly in the desktop app: **File → Open map.json** lists
every plan under `output/` with a thumbnail, and restores the scene text, style, size,
layout and terrain it was built with.

---

---

## What to say to an agent

Copy one of these. Each is written so the agent cannot mistake a plan for a render.

**Four plans, no rendering:**

> You have the D&D AI Battle Map Generator in this folder. Read `AGENTS.md` first.
> Using `agent_api.blueprint(...)` and nothing else, build four separate plans:
> a village square, a flooded crypt, a bandit camp in the woods, and a harbour with one
> moored ship. Do not call `generate` - I want the blueprints only, not painted maps.
> Tell me the output folder of each and describe what came out.

**One finished map:**

> You have the D&D AI Battle Map Generator in this folder. Read `AGENTS.md` first.
> ComfyUI is running. Build and render one finished battle map: a gothic crypt, medium
> size, with a flooded lower chamber. Use `agent_api.generate(...)`. It takes a few
> minutes - wait for it and give me the path to the PNG.

**Plan first, then decide:**

> Read `AGENTS.md`. Make three plans of a mountain pass ambush with
> `agent_api.blueprint(...)`, at three different seeds, and show me the previews. Do not
> render anything yet. When I pick one, render that one with
> `agent_api.generate_from_map(...)`.

**Edit an existing plan:**

> Read `AGENTS.md`. Open `output/village/map.json`, add a barred gate across the lane at
> the north end as an annotation, put a well in the middle of the square, then render it
> with `agent_api.generate_from_map(...)`.

The two words that decide everything are **plan** (`blueprint`, seconds, no GPU) and
**render** (`generate`, minutes, needs ComfyUI). Say which one you want and the agent has
nothing to guess.

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
