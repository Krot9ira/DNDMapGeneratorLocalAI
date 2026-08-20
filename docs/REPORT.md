# Development report

What was built, what was decided and why, and what is still open.

---

## Where the project stands

The whole pipeline runs on **Ideogram 4** locally in ComfyUI. Stage 1 turns a description
into geometry; stage 2 turns that geometry into a structured JSON caption with bounding
boxes and paints it. The desktop app, the command line and the AI agent API all drive the
same engine.

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
- **26 styles**, covering villages, town streets, castles, temples, graveyards, mines,
  sewers, roads, marshes, deserts, snow, camps, farms and ships at sea.
- **The interface wears the project's own palette** — parchment, brown ink and old gold,
  taken from `styles/_base.json`.

---

## Still open

1. **The app closed itself once**, after a successful render. The file was saved and the
   image written. No entry in the Windows event log, and it has not happened since. Worth
   watching.
2. **Prompt adherence for walls is not perfect.** The overall layout lands, but wall runs are
   sometimes redrawn a cell or two off. This is the next thing being worked on.

---

## Key files

| File | What it does |
|---|---|
| `tools/architect.py` | all geometry: rooms, corridors, walls, doors, ship, props, bleed margin |
| `tools/ideogram_prompt.py` | turns a layout into the JSON caption with bounding boxes |
| `tools/planner.py` | talks to Ollama, builds the design spec |
| `tools/workflow.py` | the ComfyUI graph for Ideogram 4 |
| `tools/agent_api.py` | the API AI agents call |
| `app/include/map_architect.h` | C++ port of the architect, kept in step with the Python |
| `app/include/ideogram_caption.h` | C++ port of the caption builder |
| `styles/_base.json` | the shared caption contract — the one sentence keeping text, creatures and grid lines out |
