# Development report

What was built, what was decided and why, and what is still open.

---

## Where the project stands

The whole pipeline runs on **Ideogram 4** locally in ComfyUI. Stage 1 turns a description
into geometry; stage 2 turns that geometry into a structured JSON caption with bounding
boxes and paints it. The desktop app, the command line and the AI agent API all drive the
same engine.

The same geometry has a second destination: an **editable Dungeondraft map**, assembled
from the user's own asset packs instead of painted. One plan, two outputs - a finished
picture for the table, and a file the GM can move a barrel in.

Full user manual: [`Manual.pdf`](Manual.pdf).

---

## Verified runs

Every one text-free, creature-free and grid-free, with the layout followed.

| Scene | Path | Output |
|---|---|---|
| Harbour with a moored ship | agent, no Ollama | `output/ideo_docks/` |
| Flooded crypt | agent, no Ollama | `output/ideo_crypt/` |
| Tavern interior | agent, no Ollama | `output/ideo_tavern/` |
| Crystal cavern | Ollama then Ideogram | `output/ideo_ollama/` |
| Annotated keep | agent, custom areas | `output/annotated_keep/` |
| Deep forest | agent | `output/deep_forest/` |

---

## The decision that mattered: bounding boxes, not ControlNet

The first pipeline drove Qwen through a ControlNet built from a rendered blueprint. It had
a trade-off with no good side: held tightly, the depth map forced the render into a flat
recoloured height map; held loosely, the layout drifted away from the plan.

Ideogram 4 is an **open-weight 9.3B model that runs locally**, trained *exclusively* on
structured JSON captions. Its bounding boxes — `[y1, x1, y2, x2]`, normalised 0–1000 — are a
trained spatial interface, not decorative text. The architect already knew the exact
rectangle of every room, wall, water body, ship and prop, so the layout is now handed over as
numbers.

That fixed both complaints at once: the layout is followed exactly, *and* the painting is
unconstrained, because nothing is pressing on the image's luminance any more.

The graph uses two UNets — the conditional model and `ideogram4_unconditional` — combined by
`DualModelGuider` for true classifier-free guidance, with `Ideogram4Scheduler` and
`SamplerCustomAdvanced`. Both model files are required; with only the first, output is pale
and low in contrast.

---

## What was fixed along the way

| Problem | Cause | Fix |
|---|---|---|
| Text baked into finished maps | area labels were drawn into the control image | labels live only in `map.json` and the human preview |
| Plans came out badly | the language model was asked for grid coordinates | the model writes prose; `architect.py` computes every coordinate |
| The app UI was broken | `ImGui::Begin` nested inside tab items | tabs are child regions; a font with the right glyph coverage |
| The editor did nothing | pan only, no drawing | brush, rectangle, props, effects, custom areas, zoom, 60-step undo |
| The ship did not look like a ship | six cells of height, nowhere to put a bow | vector hull: bow, bulwark, deck planking, hatch, mast, quarterdeck |
| Props looked like blobs | flat discs in the depth map | small clutter is described, not pinned; the renderer places it |
| Buildings inside buildings | the style asked for tiled roofs | "roof removed, interior visible" is now part of every room element |
| Cartoon look | the reference style had been over-compressed | returned to the user's `Dnd drawnlike`, minus the grid paragraph |
| Doors that were really doorways | a door was accepted anywhere | a door only survives inside a wall run; otherwise it is honestly demoted |
| Three props on a 62×51 map | density was computed per room only | global top-up from the walkable area |
| Only one render quality selectable | real NUL bytes had been written into a source string literal, ending the combo's item list | the array form of `ImGui::Combo`, which cannot lose its separators |
| A full-map effect painted in one corner | one bounding box the size of the frame is not a useful instruction | large effects are split into tiles that each have to be filled |
| Dungeondraft walls came out as disconnected hairlines | the wall texture was an `*_end` cap sprite, and each wall rectangle was converted on its own, so runs met half a cell apart | walls are traced from the wall tiles as polylines through their centres; cap sprites are never candidates |
| Dungeondraft water came out as a grid of flat slabs | one polygon per rectangle, each drawing its own border, with the shore blend set in pixels where the format counts cells | one traced polygon per body, holes nested, `blend_distance` 1.5 |
| A Dungeondraft map had no floor at all | `tiles.colors` was written as `{}`; it is one colour per cell | the array is written in full, and the floor comes from the plan's tiles rather than its room rectangles |
| Boulders laid out like a chessboard | a named region full of small things filled every other square on both axes | rock scatters on a hash of the cell, colonnades keep their ranks |

---

## Recent additions

- **Bleed margin.** Two empty cells are added outside the chosen field — never taken out of
  it. Image models are least reliable at the frame edge, so mistakes now land in blank space.
  Drawn hatched and locked in the editor.
- **Hand-editable caption.** The Render tab shows the caption in a readable view and as raw
  JSON, and can hand it over for manual editing; a hand-written caption is sent verbatim.
- **`window` material**, with its own caption element alongside doors.
- **Graphics card handover.** Before a render, Ollama is asked to unload; before planning,
  ComfyUI is asked to free its models.
- **Edits are not lost silently.** Rebuilding a plan you have edited by hand asks first and
  offers to save.
- **32 styles**, covering villages, town streets, castles, temples, graveyards, mines,
  sewers, roads, marshes, deserts, snow, camps, farms and ships at sea.
- **The interface wears the project's own palette** — parchment, brown ink and old gold,
  taken from `styles/_base.json`.

---

## Still open

0. **Dungeondraft caves.** A cave scene writes its `cave.bitmap` and also emits ordinary
   walls. The bitmap is what draws rock in Dungeondraft, so the walls may be redundant
   there. Open one and decide.
1. **The app closed itself once**, after a successful render. The file was saved and the
   image written. No entry in the Windows event log, and it has not happened since. Worth
   watching.
2. **Prompt adherence.** Five test renders of the same harbour plan, each changing one thing:

   | Round | Change | Result |
   |---|---|---|
   | 1 | wall runs given their own bounding boxes | walls straight and continuous instead of wandering; a row of invented archways along one long wall |
   | 2 | the caption states how many doors and windows exist in total | invented archways stopped; layout came back mirrored and over-subdivided |
   | 3 | each logical wall merged into one rectangle; open ground described; "not symmetrical" stated | open quay stayed open; symmetry reduced, not gone |
   | 4 | buildings as footprints, open ground as real blocks, every wall run kept | walls covered 98% of the plan - and at 45 elements the layout came back mirrored, interiors subdivided, and **two human figures appeared** in a map whose caption forbids them |
   | 5 | **fewer elements, not more**: buildings as whole named objects, only free-standing wall runs, no room described twice, ban repeated last. 24 elements, 10 KB | three separate buildings of the right sizes, one door each, open ground genuinely open, margin clean, no people |

   The turning point was drawing the caption's own bounding boxes back onto the plan
   (`scratchpad/bbox_overlay.py` in the working notes). It showed the wall boxes landing
   exactly where they should - and the open ground carrying no boxes at all. The renderer was
   not ignoring the walls; it was filling the silence everywhere else. Adding more boxes made
   that worse, because the caption has a finite attention budget and sixteen near-identical
   wall runs spent it.

   Still imperfect: room proportions drift by a cell or two, and a room the plan leaves open
   on one side may come back closed with a door.

---

### What the render tests established

Every one of these came from a render that came back wrong, and each is now enforced in the
caption builder rather than left to whoever writes the spec. The full list, with the
reasoning, is in `AGENTS.md` under **How the renderer reads what you give it**.

| Lesson | Where it lives now |
|---|---|
| A rectangle always means "draw this here"; there is no rectangle for "nothing" | documented; the builder no longer emits empty-region boxes |
| Rectangles hugging all four walls join into an inner wall | documented for spec writers |
| Undescribed space is filled with something invented | open ground is described as real blocks |
| A single-area building must be told it is one undivided room | `ideogram_prompt.py`, and the C++ port |
| Negations are weak; positive statements and local counts hold | wall and door wording |
| Describe the top face, never the front; never mention ceilings | `viewpoint_note` in the shared contract |
| Past ~40 elements adherence collapses across the board | `MAX_ELEMENTS = 24`, priority tiers |
| A style's own text outweighs the caption's elements and can contradict the plan | `style_warnings()`, reported by `build_map` |

---

## Key files

| File | What it does |
|---|---|
| `tools/architect.py` | all geometry: rooms, corridors, walls, doors, ship, props, bleed margin |
| `tools/ideogram_prompt.py` | turns a layout into the JSON caption with bounding boxes |
| `tools/planner.py` | talks to Ollama, builds the design spec |
| `tools/workflow.py` | the ComfyUI graph for Ideogram 4 |
| `tools/agent_api.py` | the API AI agents call |
| `tools/dungeondraft_indexer.py` | scans the user's packs into `data/assets.db` |
| `tools/dungeondraft_matcher.py` | picks an asset for a wall, a floor, a terrain slot or a prop |
| `tools/dungeondraft_assembler.py` | writes the `.dungeondraft_map`: walls, portals, water, tiles, terrain, caves |
| `tools/dungeondraft_foundry.py` | generates a prop the library has no answer for, and packs it |
| `docs/dungeondraft_map_format.md` | the format, verified against real map files |
| `app/include/map_architect.h` | C++ port of the architect, kept in step with the Python |
| `app/include/ideogram_caption.h` | C++ port of the caption builder |
| `styles/_base.json` | the shared caption contract — the one sentence keeping text, creatures and grid lines out |
