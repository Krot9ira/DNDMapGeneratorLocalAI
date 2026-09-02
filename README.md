# D&D AI Battle Map Generator

[![CI](https://github.com/Krot9ira/DNDMapGeneratorLocalAI/actions/workflows/ci.yml/badge.svg)](https://github.com/Krot9ira/DNDMapGeneratorLocalAI/actions/workflows/ci.yml)
[![Licence: MIT](https://img.shields.io/badge/licence-MIT-blue.svg)](LICENSE)

Describe a place in one sentence and get a finished top-down battle map you can drop straight
into Roll20, Foundry or a printer.

```
"a stone quay with one large ship moored alongside and a gangway ashore"
        ↓  stage 1 — plan the place, compute the geometry
        ↓  stage 2 — paint it with Ideogram 4 running locally
   output/my_map/battlemap.png
```

Everything runs on your own machine. No accounts, no API keys, no upload of anything.

- **Never rendered:** text, living creatures, grid lines. Your VTT draws the grid, you place
  the tokens.
- **A bleed margin** of empty ground is added around your field, so the model's worst
  guessing happens off the map instead of in a room.
- **Desktop app** for people, **command line and Python API** for AI agents — same engine.
- **Full manual for absolute beginners:** [`docs/Manual.pdf`](docs/Manual.pdf).

---

## Table of contents

1. [What it actually does](#what-it-actually-does)
2. [Hardware requirements](#hardware-requirements)
3. [Install the prebuilt release](#install-the-prebuilt-release)
4. [Your first map](#your-first-map)
5. [The app, tab by tab](#the-app-tab-by-tab)
6. [The editor in detail](#the-editor-in-detail)
7. [Styles](#styles)
8. [Dungeondraft map assembly & asset packs](#dungeondraft-map-assembly--asset-packs)
9. [Command line](#command-line)
10. [AI agents](#ai-agents)
11. [Build from source & developer guide](#build-from-source--developer-guide)
12. [How it works, and why](#how-it-works-and-why)
13. [Nuances worth knowing](#nuances-worth-knowing)
14. [Troubleshooting](#troubleshooting)
15. [Output files](#output-files)
16. [Repository layout](#repository-layout)
17. [Contributing](#contributing)
18. [Licence](#licence)

---

## What it actually does

Two stages. Almost every problem you will ever have belongs to one of them.

**Stage 1 — the brain.** Your description becomes a *design spec*: what kind of place this
is, which areas it has, what it is made of, what is lying around. A deterministic architect
then turns that spec into exact geometry — rooms, corridors, walls, doors, water, a ship
hull, prop positions.

The spec can come from three places, and they are interchangeable:

| Source | How | Needs |
|---|---|---|
| Local language model | Create tab, or `pipeline.py plan` | Ollama |
| An AI agent | `agent_api.generate({...})` | nothing |
| You | Editor tab, or a hand-written spec JSON | nothing |

What the language model contributes is **words**, never coordinates. Your one line becomes
paragraphs of concrete detail — wet cobbles with tar stains rather than "stone", a
harbourmaster's office rather than "a room" — and the caption builder wraps that wording
around geometry it computed itself. That split is why a broken layout is not representable,
and why an agent can replace Ollama outright.

**Stage 2 — the painter.** The finished layout is serialised as a structured JSON caption
with a bounding box for every object and handed to **Ideogram 4** running locally in
ComfyUI. What comes back is the map.

---

## Hardware requirements

The app itself is a 2 MB native Windows program that costs nothing to run. Everything
demanding happens inside ComfyUI.

| | Minimum | Recommended | Reference machine |
|---|---|---|---|
| OS | Windows 10 x64 | Windows 11 x64 | Windows 11 Pro |
| GPU | NVIDIA, 8 GB VRAM | NVIDIA, 12 GB VRAM or more | RTX 3080 Ti, 12 GB |
| System RAM | 32 GB | 64 GB | 64 GB |
| Free disk | 35 GB | 50 GB | — |
| CPU | any modern x64 | — | — |

Why the RAM matters more than usual: the pipeline holds a 9.9 GB text encoder and **two**
8.6 GB model files. They do not fit in consumer VRAM together, so ComfyUI keeps most of it
in system RAM and streams it to the GPU. With 32 GB it works; with 64 GB it stops thinking
about it. With 16 GB expect swapping and pain.

**8 GB VRAM** works with ComfyUI's offloading (start it with `--lowvram` if it struggles),
just slower. **AMD, Intel Arc and Apple** are untested — the app talks plain HTTP, so if
ComfyUI runs Ideogram 4 on your hardware the app will drive it, but nobody has checked.

A render is minutes, not seconds. The first one after starting ComfyUI is much slower than
the rest because ~27 GB of weights come off disk. Step counts by preset: **Turbo** 12,
**Default** 20, **Quality** 48.

### Software

| | Purpose | Required |
|---|---|---|
| [ComfyUI](https://github.com/comfyanonymous/ComfyUI) | renders the image | yes |
| Ideogram 4.0 weights | the model that paints | yes |
| [Ollama](https://ollama.com) | plans the scene from your description | optional |
| Python 3.10+ and Pillow | command line and agent tooling | optional |

Without Ollama the app still works end to end — *Blueprint only* builds the layout instantly
and the Editor lets you shape it by hand. You only lose "type one sentence, get a plan".

---

## Install the prebuilt release

### 1. Get the app

Download the newest `DndBattlemapGenerator-*.zip` from the
[Releases](../../releases) page and unpack it anywhere — Desktop is fine. There is no
installer and nothing is written outside the folder.

```
DndBattlemapGenerator/
  DndBattlemapGenerator.exe   the app
  config.json                 service addresses and model filenames
  styles/                     style library, editable in the app
  presets/                    ready-made maps
  output/                     your results land here
  tools/                      command line + AI agent API
  docs/Manual.pdf             the full manual
  AGENTS.md                   instructions for AI agents
  LICENSE, THIRD_PARTY.md     licences
```

If Windows SmartScreen objects: *More info → Run anyway*. The binary is unsigned because
code-signing certificates cost money.

### 2. Install ComfyUI

Either the [desktop build](https://www.comfy.org/download) or the portable/manual install
from the [repository](https://github.com/comfyanonymous/ComfyUI). Any recent version is
fine; no custom nodes are needed — the Ideogram 4 nodes are built in.

Start it and confirm <http://127.0.0.1:8188> opens in a browser.

### 3. Download the four model files

Ideogram 4.0 is an open-weight model. Get the ComfyUI-repackaged files — the ComfyUI model
browser lists them, or search Hugging Face for these exact filenames — and put each one in
the folder shown:

| File | Size | Goes in |
|---|---|---|
| `ideogram4_fp8_scaled.safetensors` | 8.6 GB | `ComfyUI/models/diffusion_models/` |
| `ideogram4_unconditional_fp8_scaled.safetensors` | 8.6 GB | `ComfyUI/models/diffusion_models/` |
| `qwen3vl_8b_fp8_scaled.safetensors` | 9.9 GB | `ComfyUI/models/text_encoders/` |
| `flux2-vae.safetensors` | 0.3 GB | `ComfyUI/models/vae/` |

≈ 27.5 GB in total.

**Both `ideogram4` files are required.** One is the conditional model, the other the
unconditional one; the graph combines them to get true classifier-free guidance. With only
the first, output is washed out and vague.

If you use different filenames or quantisations, put yours in the **Settings** tab (or in
`config.json`) — nothing is hard-coded.

### 4. Install Ollama (optional)

Install [Ollama](https://ollama.com) and pull a model that can follow a JSON schema:

```bash
ollama pull qwen3:8b
```

Then set that name in the app's **Settings** tab. The shipped `config.json` names a larger
model; whatever you pull, the name must match exactly. Bigger models write richer scenes but
compete with ComfyUI for VRAM — if both are on the same GPU, prefer a small planner.

### 5. Check the wiring

Start `DndBattlemapGenerator.exe`, open **Settings**, press **Test both connections**.

- `ComfyUI: ready` — you can render.
- `Ollama: offline` — fine, unless you want it to write plans for you.

The same status line sits at the bottom of the window at all times.

### 6. Python tooling (optional)

Only needed for the command line and the agent API:

```bash
pip install pillow
```

---

## Your first map

1. **Create** tab.
2. Type a scene: `a flooded crypt with cracked sarcophagi and standing water`.
3. Pick a look from the style cards.
4. Set width and height, or press one of the size buttons. *Medium* is a normal fight.
5. **MAKE MY BATTLE MAP**, then wait.

The plan appears within seconds, the painted map after a few minutes. Both land in
`output/<name>/`.

In a hurry, or Ollama is not installed? Press **Blueprint only** — the layout is built
instantly and locally, and you can render it later from the **Render** tab.

---

## The app, tab by tab

**Create** — the scene description, the style grid, width and height in cells (10–150 each),
layout, ground clutter, prop density. Two buttons: full run, or blueprint only.

**Editor** — the plan as an editable grid. Paint materials, place props, describe your own
objects, add effects. Everything here is what actually gets rendered. Building a new plan on
top of edits you made by hand asks first, and offers to save what you have.

**Render** — quality preset, guidance, output resolution, seed, and the caption that will be
sent, shown both readably and as raw JSON. **Edit by hand** takes the caption over: whatever
you type is sent verbatim. Renders whatever is currently in the Editor, so you can re-render an
edited plan without replanning it.

**Styles** — edit any style file in place: materials, palette, lighting, aesthetics. Also
the shared caption contract that every style inherits.

**Settings** — service addresses, model filenames, planner model and temperature. Written to
`config.json` and shared with the command line tools.

**File → Open map.json** lists every plan under `output/` as a thumbnail grid, so a map
built by an agent or the command line can be reviewed and edited in the app. Opening one
restores the scene text, style, size, layout and terrain it was made with.

---

## The editor in detail

### Tools

| Tool | What it does |
|---|---|
| **Look around** | pan and zoom only, changes nothing — the safe mode |
| **Brush** | paint the selected material; brush size below |
| **Rectangle** | fill a rectangle: press, drag, release |
| **Place prop** | drop a catalogue prop, or your own custom object |
| **Erase prop** | remove a prop |
| **Custom area** | drag a rectangle and describe in your own words what belongs there |
| **Effect** | drag a rectangle and lay an atmospheric effect over it |

Right mouse or middle mouse drags the view; the wheel zooms.

### Materials

`floor`, `wall`, `door`, `window`, `water`, `pit`, `rubble`, `vegetation`, `bridge`, `stairs`,
and `void` which erases back to nothing.

Doors and windows are load-bearing: each one is pinned in the caption with its own bounding
box, because without that the renderer puts openings wherever it likes. A door only survives
inside a wall run — paint one in open floor and it is honestly demoted to a plain opening.

**Rebuild walls** re-derives every wall from the floor you have painted. Use it after big
changes — it is how the generated maps get their walls in the first place.

### Props

A scrolling icon catalogue. Hovering a prop says what it is and how it plays — *full cover*,
*difficult ground*, *light source*.

Tick **Custom object** and you can name and describe anything the catalogue has no word for.
Then choose how much licence the model gets:

- **No — exactly as written** — draw precisely that, add nothing.
- **A little** — fill in fitting detail.
- **Freely** — treat it as a starting point.

### Custom areas

The strongest instrument in the app. Drag a rectangle, give it a name and a description, and
that text goes into the caption **at top priority, with its coordinates**. Use it for a
barred gate, a collapsed section of wall, a specific door, a mosaic floor — anything that
must be *there* and must look like *that*.

### Effects

A layer above everything else: fire, embers, smoke, fog, mist, fireflies, arcane glow, holy
light, poison gas, blood, ice, webs, sparks, ash, steam, shadow — plus your own. Effects
never change what a square is made of or whether you can walk through it; they are light,
smoke and weather painted over the finished map. **Strength** sets how strongly the effect
reads.

### Hover and right-click

Hovering any square names what stands on it and gives its coordinates. **Right-clicking**
opens a menu for whatever is under the cursor: rename it, rewrite its description, change how
freely the model may embellish it, or delete it. On an empty square it offers to pick that
material into the brush.

Right-click *and drag* still pans, so the menu only opens on a click that stayed put.

---

## Styles

Thirty-two of them, one JSON file each in `styles/`, covering the places a campaign actually
visits:

| | |
|---|---|
| **Settlement** | `village_hamlet`, `farmstead` |
| **Urban** | `city_streets`, `city_district`, `city_townhouse`, `city_harbour`, `cyberpunk_street` |
| **Fortress** | `castle_keep` |
| **Dungeon** | `classic_dungeon`, `gothic_crypt`, `sunken_temple`, `sewer_tunnels` |
| **Underground** | `underdark_cavern`, `natural_cave`, `abandoned_mine`, `volcanic_lair` |
| **Sacred** | `grand_temple`, `graveyard` |
| **Interior** | `cozy_tavern`, `arcane_library`, `merchant_hall` |
| **Wilderness** | `lush_forest`, `marsh_bog`, `mountain_pass`, `frozen_pass`, `desert_ruins`, `coastal_ruins`, `bandit_camp`, `caravan_camp`, `caravan_camp_night` |
| **Nautical** | `pirate_deck` |
| **Sci-fi** | `scifi_derelict` |

A style carries materials, palette, lighting, aesthetics, a default layout and a default prop
set. `styles/_base.json` holds the fragments every style inherits — including the single
sentence that bans text, creatures and grid lines from the image.

To add your own: copy any style file, change the `id`, and it appears in the app on restart.
The Styles tab edits all of it without leaving the program.

An agent can do the same, and should: when the library holds nothing of the kind of place
it was asked for, `agent_api.create_style(...)` writes one, after checking that it will not
spoil every map it paints. Each style records where it came from - `shipped` with the
program, `user` if you wrote it, `agent` if the AI did - and the style cards carry a
**YOURS** or **AI** badge accordingly, so it is always clear which is which.

---

## Dungeondraft map assembly & asset packs

In addition to painted battle map images via Ideogram, the pipeline can assemble native,
fully editable **`.dungeondraft_map`** files directly from architectural map plans.

```
"a flooded crypt with sarcophagi"
        ↓  stage 1 — plan layout & geometry
        ↓  stage 2 — match assets & build Godot 3 vector structures
   output/flooded_crypt/flooded_crypt.dungeondraft_map
```

### Key features

- **Native Dungeondraft map generation:** builds walls, portals (doors and windows cut into
  the wall they interrupt), floor tiles, water bodies, terrain splatmaps (4 samples per cell
  per axis), natural cave bitmask arrays (`cave.bitmap` LSB-first), lights, and placed
  objects.
- **Vector geometry traced from the tile grid:** a run of wall tiles becomes one polyline,
  pushed onto the grid lines so no square is left cut in half, with corners that meet and
  doorways cut into a continuous wall as real portals; a solid mass of rock becomes its
  outline. Each body of water is a single polygon traced along the cell edges, with
  enclosed gaps nested as holes.
- **No bleed margin:** the empty ring added for the diffusion model is trimmed off, so the
  map opens at exactly the size the field was planned at.
- **Ground painted by what it is:** floor tiles under rooms, planking under decks and
  gangways, and a terrain splat that follows the plan's undergrowth, rubble and lake beds
  instead of one flat texture over the whole field.
- **High-throughput multi-core indexer:** parallelizes asset extraction, thumbnail generation,
  and content hashing across up to 32 CPU threads.
- **Stock and custom pack indexing:** indexes built-in assets from `Dungeondraft.pck` and
  custom `.dungeondraft_pack` archives into a local SQLite database (`data/assets.db`).
- **Vision model enrichment:** automatically classifies texture shapes, footprints, styles,
  and controlled vocabulary tags using a local vision model (`gemma4:12b` in Ollama) with
  dedicated scopes (`stock`, `custom`, `all`).
- **Prop Foundry (`tools/dungeondraft_foundry.py`):** synthesizes missing standalone props on
  transparent backgrounds and packages them into a native Godot 3 `.dungeondraft_pack` archive.
- **Format documentation:** full reverse-engineered format specification is documented in
  [`docs/dungeondraft_map_format.md`](docs/dungeondraft_map_format.md).

### Managing asset packs & vision cataloguing

```bash
# Scan and index built-in assets and custom packs across all CPU cores:
python tools/dungeondraft_indexer.py scan --threads 32

# Show current library metrics (indexed packs, texture categories):
python tools/dungeondraft_indexer.py stats

# Validate database integrity, verify hashes, and detect suspect enrichments:
python tools/dungeondraft_indexer.py validate

# Run a quick quality check on 200 sampled assets before a full pass:
python tools/check_enrichment_quality.py --model gemma4:12b --sample 200

# Run vision enrichment on stock assets only:
python tools/dungeondraft_enrich.py --scope stock --model gemma4:12b

# Run vision enrichment on custom packs only:
python tools/dungeondraft_enrich.py --scope custom --model gemma4:12b

# Generate a missing custom prop and assemble into custom pack:
python tools/dungeondraft_foundry.py generate --prop banner --style gothic_crypt
```

---

## Command line

Needs Python 3.10+ and Pillow. Run from the release folder or the repository.

```bash
python tools/pipeline.py styles
```

```bash
python tools/pipeline.py plan "flooded crypt with sarcophagi" --style gothic_crypt --size medium
```

```bash
python tools/pipeline.py dungeondraft output/my_map/map.json
```

```bash
python tools/pipeline.py auto "a harbour with one large moored ship" --style city_harbour
```

```bash
python tools/pipeline.py generate output/my_map/map.json
```

```bash
python tools/pipeline.py preview output/my_map/map.json
```

```bash
python tools/pipeline.py validate output/my_map/map.json
```

| Command | Stage | Description | Needs Ollama |
|---|---|---|---|
| `styles` | — | list available styles and presets | no |
| `plan` | 1 | create design spec & geometry from description | yes |
| `build` | 1 | build geometry from spec file (no LLM) | no |
| `preview` | 1 | redraw the human blueprint preview | no |
| `dungeondraft` | 2 | assemble native `.dungeondraft_map` and report | no |
| `generate` | 2 | paint battle map with Ideogram 4 in ComfyUI | no |
| `auto` | 1 then 2 | plan and render sequentially | yes |
| `caption` | 2 | print the caption a plan would be rendered from, and stop | no |
| `validate` | — | check and repair a `map.json` | no |

Useful flags: `--cols` / `--rows` for an exact grid, `--seed` for a repeatable layout,
`--out` for the output directory, `--sketch <image>` to hand the planner a rough layout
picture (needs a vision-capable Ollama model).

---

## AI agents

An agent that is already a capable model does not need Ollama — it writes the spec itself,
which is faster, frees VRAM for the renderer, and gives direct control over the scene.

Full guide: [`AGENTS.md`](AGENTS.md).

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
    "prop_density": "high",
    "rooms": [
        {"label": "Cargo Warehouse", "size": "l", "props": ["crate", "barrel", "cart"]},
        {"label": "Harbourmaster",   "size": "s", "props": ["table", "bookshelf"]},
    ],
})

print(result["images"])    # finished PNG paths
print(result["out_dir"])   # everything else
```

**The one rule:** describe the place, never compute coordinates. The architect owns every
spatial decision. The single exception is annotating a rectangle you want controlled exactly.

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

---

## Build from source & developer guide

### 1. Prerequisites

- Visual Studio 2022 or newer with the **Desktop development with C++** workload
  (developed against Visual Studio 18 2026; VS 2022 works, the code is standard C++20)
- Windows 10/11 SDK
- CMake 3.20+
- Python 3.10+ with `pillow` and `requests`:
  ```bash
  pip install pillow requests
  ```

No external C++ package manager is required: Dear ImGui, nlohmann/json and stb are vendored in `app/vendor/`.

### 2. Building

#### Option A: One-step build script (PowerShell)
```powershell
.\build.ps1 -Zip
```
`-Clean` deletes the build folder first; `-Zip` creates a distribution archive.

#### Option B: CMake directly
```bash
cmake -B build -S . -A x64
cmake --build build --config Release
```

Two outputs are produced:
- `bin/Release/DndBattlemapGenerator.exe` — the development build. It walks up to the repository root to share styles with Python tools.
- `dist/DndBattlemapGenerator/` — the complete, self-contained shippable release folder with all binary, data, tool, and documentation assets.

The packaging step deletes and rebuilds `dist/`, so close the packaged app before building or the copy will fail with a file-in-use error.

### 3. Running automated test suites

The repository contains regression suites for architecture, prompting, format parity, and Dungeondraft assembly:

```bash
# 1. Dungeondraft map assembly regression test across 13 distinct scenes:
python tools/check_dungeondraft.py

# 2. Style caption linting across all 32 prompt styles:
python tools/check_captions.py

# 3. Scene layout and geometry validation:
python tools/check_scenes.py

# 4. Caption parity, field by field and key order, between C++ engine and Python tools:
python tools/check_caption_parity.py

# 5. Enrichment sample benchmark and HTML contact sheet:
python tools/check_enrichment_quality.py
```

### 4. Layout of the code

- `app/src/main.cpp` — entry point: resolve the project root, open a window, run the frame loop.
- `app/src/core/` — Win32 window, DirectX 11 device, process launching, file dialogs.
- `app/src/services/` — connection monitoring, the render pipeline driver.
- `app/src/ui/` — the ImGui context, texture loading, the map canvas, and one file per tab.
- `app/include/map_architect.h` — C++ header-only port of `tools/architect.py`.
- `app/include/ideogram_caption.h` — C++ header-only port of `tools/ideogram_prompt.py`.
- `tools/dungeondraft_pck.py` — Godot 3 GDPC binary parser and `.stex` / WebP texture decompressor.
- `tools/dungeondraft_db.py` — SQLite database abstraction and image content hash / thumbnail generator.
- `tools/dungeondraft_indexer.py` — Dungeondraft pack scanner and indexer.
- `tools/dungeondraft_enrich.py` — Ollama vision model (`gemma4:12b`) schema-constrained enrichment.
- `tools/dungeondraft_matcher.py` — Semantic asset matcher.
- `tools/dungeondraft_assembler.py` — Native `.dungeondraft_map` generator and validator.
- `docs/dungeondraft_map_format.md` — Full format documentation for `.dungeondraft_map`.

---

## How it works, and why

### The language model never sees a coordinate

Early versions asked the model for rectangles and got overlapping rooms, walls enclosing
nothing, and doors in the middle of open floor. Language models are good at describing
places and bad at grid arithmetic.

So the model only ever writes prose. `tools/architect.py` does every spatial calculation:
BSP room packing, L-shaped corridors, a nearest-neighbour spanning tree so every room is
reachable, walls derived from the floor by 8-neighbour dilation, cellular-automata caverns,
and greedy rectangle decomposition to turn a painted grid back into editable zones. A broken
layout is not representable.

### Bounding boxes, not a ControlNet image

Ideogram 4 is an open-weight model trained *exclusively* on structured JSON captions, and its
bounding boxes — `[y1, x1, y2, x2]`, normalised 0–1000, top-left origin — are a trained
spatial interface, not decorative text. The architect already knows the exact rectangle of
every room, door, water body, ship and prop, so the layout is handed over as numbers.

That escapes the failure the ControlNet path could not: held tightly, a depth map forced the
render into a flat recoloured height map; held loosely, the layout drifted. With bounding
boxes the layout is exact *and* the painting is unconstrained.

### What is never in the picture

- **Text.** No labels, no numbers, no compass rose. Area names live in `map.json` and in the
  human preview only.
- **Creatures.** The GM places tokens afterwards.
- **A grid.** You choose the size in cells, but no grid lines are baked in — your VTT draws
  its own at whatever scale it likes.

Ideogram takes **no negative prompt**, so none of this can be forbidden the usual way. The
ban is one positive sentence in `styles/_base.json`, and that sentence is the only thing
holding the line. Edit it carefully.

---

## Nuances worth knowing

**A door only exists inside a wall.** If you paint a door with open floor on both sides, the
app either grows walls around it or honestly demotes it to a plain opening — because that is
what it is. Paint the wall first, then the door into it, or press *Rebuild walls*.

**Small clutter is deliberately not pinned.** Load-bearing things — walls, water, the ship,
doors, your custom objects and custom areas — get exact bounding boxes. Loose barrels and
crates are described but left to the renderer, because pinning fifty small objects makes the
painting stiff and worse. If a specific prop matters, place it as a **custom object** or mark
it with a **custom area**.

**Caption budget.** At most 40 elements carry a bounding box, in priority order: annotations
and effects, then the ship, doors and custom props, then terrain and rooms, then catalogue
props. Beyond that, things are mentioned without coordinates. Larger maps therefore describe
more and pin proportionally less.

**Buildings are drawn roofless.** You are looking down into rooms. Ships are drawn from
above, decks visible, and lower decks only if you ask for them.

**Seeds.** The same seed with the same spec gives the same layout. `--seed` on the command
line, and the seed field in the Render tab. `-1` means random.

**Resolution** comes from the aspect ratio of your grid and the target megapixels in
Settings, snapped to a multiple of 16. A 150×150 grid is a big image and a long render — try
the shape at *medium* first.

**The bleed margin is added, not taken.** Ask for 25×19 and you get 25×19 squares to work
with; the picture is 29×23 with a blank rim. It is hatched and locked in the editor. Image
models are least reliable at the very edge of a frame, so the margin is where their mistakes
go instead of into the corner of a room — and it makes the result read as a finished printed
sheet rather than something cropped.

**The two services take turns with the card.** Neither the planner nor the painter fits in
consumer VRAM alongside the other, so before a render Ollama is asked to unload, and before
planning ComfyUI is asked to free its models. Nothing to configure; it just costs a few
seconds at each handover.

**Nothing phones home.** No telemetry, no network access except to `127.0.0.1`.

**The app is a client.** If ComfyUI is busy with another job, your render waits in its queue.

---

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `ComfyUI: offline` | ComfyUI is not running, or is on another port. Start it; check the address in Settings. |
| Render fails immediately | A model filename in Settings does not match a file in `ComfyUI/models/`. Compare them character by character. |
| Image is washed out, vague, low contrast | `ideogram4_unconditional_*` is missing or misnamed, so guidance has nothing to work against. |
| Out of memory during render | Close other GPU applications; start ComfyUI with `--lowvram`; drop the target megapixels in Settings. |
| `Ollama: offline` but you installed it | Ollama is not running, or the model name in Settings differs from what `ollama list` shows. |
| Planning is slow or times out | The planner model is large and sharing the GPU with ComfyUI. Pull a smaller one, or raise the timeout. |
| The map ignores something you asked for | It was probably in the description but not in the plan. Check the Editor: what you see there is what gets rendered. Pin the missing thing with a custom area. |
| A door came out as a doorway | It had no wall around it. See the door rule above. |
| The map looks empty | Raise prop density on the Create tab, or paint what you want in the Editor. |
| Text appeared in the image | Something re-introduced it into the caption. Check the `forbidden_suffix` line in `styles/_base.json`. |
| The build fails copying to `dist/` | The packaged app is running. Close it and build again. |

---

## Output files

Everything for one map lands in `output/<name>/`.

| File | What it is |
|---|---|
| `battlemap*.png` | **the finished map** (painted image) |
| `*.dungeondraft_map` | **editable Dungeondraft map file** |
| `*.report.json` | assembly report: placed walls, matched assets, referenced packs |
| `preview.png` | labelled blueprint, for humans only — never rendered |
| `map.json` | the layout; reopen and re-render it any time |
| `caption.json` | the structured caption with bounding boxes sent to Ideogram |
| `spec.json` | the design spec stage 1 produced |

`map.json` is the durable artefact. Keep it and you can re-render at any size, in any style,
with any seed, forever.

---

## Repository layout

```
app/          C++ sources (ImGui + DirectX 11) and vendored libraries
  src/core       Win32 window, D3D11 device, process launching, file dialogs
  src/services   connection monitoring, the render pipeline driver
  src/ui         ImGui context, map canvas, one file per tab
  include/       header-only ports of the architect and the caption builder
tools/        Python: architect, caption builder, ComfyUI client, CLI, agent API
styles/       style library, one JSON per style
presets/      ready-made maps
docs/         user manual, development report, Dungeondraft format reference
packaging/    files that ship with a release
reference/    ComfyUI workflow export kept for reference
bin/          development build
dist/         self-contained package, assembled on every build
data/         Dungeondraft asset index and thumbnails, built on your machine
output/       generated maps
```

---

## Contributing

Bug reports, scenes that came out wrong, styles and code are all welcome.
[`CONTRIBUTING.md`](CONTRIBUTING.md) has how to build it, the five checks CI
runs, and the two things this project will bite you for — the geometry engine
exists twice, once in Python and once in C++, and both copies have to move
together.

The most useful bug report is a map that came out wrong with its `map.json`
attached: that is the plan, and it is what makes the problem reproducible on
somebody else's machine.

---

## Licence

MIT — see [`LICENSE`](LICENSE). Use it, fork it, sell what you build with it; just keep the
copyright notice and credit **Krot9ira** (<https://github.com/Krot9ira>).

The app statically links Dear ImGui, nlohmann/json and stb, and talks to ComfyUI, Ideogram 4
and Ollama over HTTP without bundling any of them. Every one of those, with its author and
licence, is listed in [`THIRD_PARTY.md`](THIRD_PARTY.md), which also ships inside the release
folder.
