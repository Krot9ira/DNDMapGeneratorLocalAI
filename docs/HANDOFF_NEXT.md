# Handoff: prompt-adherence work, current state

> Read `AGENTS.md` first — "How the renderer reads what you give it" is the rulebook every fix below builds on.

## Current state — 2026-08-26, ready to hand to next agent

**Goal (user repeats every session):** battlemaps from a text description must match the description structurally — no invented walls/rooms, no missing entrances — and be **strictly top-down orthographic, zero tilt**. Tilt / 3/4-view at the top edge is the **critical** defect, because a wall drawn as a face covers grid squares a token must stand on. See memory `top-down-view-is-critical`.

**Both ports must be changed together:** `tools/*.py` and `app/include/*.h` are two mirrors of the same logic. After any caption/architect change: `python tools/check_caption_parity.py` must be green (plus a fresh `cmake --build build --config Release` if you touched `.h`).

**Tree:** clean on top of `8087a1d` (`Clarify undivided spaces and open plaza in burning quarter scene`), `1427a77` (`Only describe a door as in an interior wall when both rooms are inside the same building`), `916091e` (`Preserve enclosed room attribute in architect and clean street props`), `d172c6e` (`Spilled masonry no longer names the wall that fell`), and `55569cd` (`The caption stops naming walls it does not want`). `output/` is gitignored — renders are not committed. Ollama and ComfyUI `127.0.0.1:8188` are up.

### burning_quarter — the active scene

Scene file `tools/scenes/burning_quarter.json` was **replanned 48×40** to satisfy the user's two new constraints (plaza ≥15×15, fountain decorated):

- 6 areas: `Stone House` (4,4,12,9) enclosed, `Burning Tenement` (18,4,8,8) enclosed, `Burnt-Out House` (30,4,10,5) enclosed, `Fountain Square` (28,10,16,16) open (16×16) with benches/bushes/trees/lamps loose about it, `Burning Street` (4,26,40,8) open, `Burning Warehouse` (6,34,10,4) enclosed.
- Annotations (exact): `Toppled Waggon` (17,29,7,4), `Stone Fountain` (33,15,5,5) + water zone (33,15,5,5), `Burning Gallery` (15,6,4,2), `City Gate` (19,33,10,3) over wall zone (19,34,10,1) + door (21,34,2,1) gap, `Fallen Beam` (36,29,6,3), `Spilled Masonry` (7,30,6,4), `Burning Cobbles` (26,28,7,3) `some`.
- Seed `7`, border `2`, style `city_streets`. Built map `output/scene_burning_quarter/map.json` (grid 52×44 stored, 48×40 playable). Caption `output/scene_burning_quarter/caption.json` has `compositional_deconstruction` with keys `bbox/desc/type`.

**Fixes landed in this run:**
1. **Wall-noun purge in annotations (d172c6e):** In `tools/scenes/burning_quarter.json` annotation `Spilled Masonry`, changed description from `"… where the wall of a building has come down …"` to `"A heap of broken stone and charred timber spilled flat across the cobbles, spread wide and still smoking"`.
2. **Room `enclosed` attribute preservation (916091e):** Fixed bug in `tools/architect.py` `normalize_spec()` where `entry["enclosed"]` was dropped when normalizing rooms, causing enclosed buildings (like `Burning Tenement`) to fall back to heuristics and be misclassified as outdoor areas. Also reduced `Burning Street` props from `[cart, rubble, rubble]` to `[cart]` to eliminate auto-tiled rubble pens across the open street.
3. **Exterior vs Interior Door Wording (1427a77):** In both `tools/ideogram_prompt.py` and `app/include/ideogram_caption.h`, `_door_place` / `DoorPlace` previously declared `"In the interior wall between A and B"` whenever two areas were adjacent across a doorway, even if one area was an outdoor plaza or street. This strongly cued the image model to treat outdoor squares (like `Fountain Square`) as indoor rooms enclosed by interior walls. Updated both ports with `_host_for_area` / `hostForArea` to only emit `"In the interior wall between..."` if both rooms share the same host building. Doors between indoor rooms and outdoor squares/streets now correctly describe `"In the outer wall of the building, the way in and out of <room>"`.
4. **Undivided space and plaza clarity (8087a1d):** Refined room descriptions in `burning_quarter.json` to state undivided spaces clearly (e.g. `"one undivided space"`, singular floor) to eliminate unintended room subdivisions.

**Render series (seed 7):**
- `battlemap_00112_.png` & `battlemap_00113_.png`: `Stone House` and `Burning Tenement` restored to two distinct buildings joined by a door; street props positioned cleanly; identified interior wall door caption bug on `Fountain Square`.
- `battlemap_00114_.png`: First render with door outer wall fix. Doors on `Burnt-Out House` now correctly described as outer wall. `Fountain Square` fountain stands free at center.
- `battlemap_00115_.png`: Render with undivided space refinements. Layout is clean, top edge orthographic, street features and square clearly delineated. All 14 parity tests, 13 scene checks, caption lints, and layout generator tests pass 100%.

### Verification tools (run before trusting any change)

```
python tools/check_scenes.py          # builds/validates all 13 test scenes
python tools/check_captions.py        # lints every style for side-on words (negation-aware)
python tools/check_caption_parity.py  # app vs tools caption diff — MUST be clean
python tools/check_layouts.py         # generator sanity across layouts/sizes/seeds
```

### The 13 test scenes (`tools/scenes/*.json`)

Regression suite — prompts in `tools/scenes/prompts/`.

| Scene | Last render | Verdict |
|---|---|---|
| magic_hall | battlemap_00092_ (pre-purge) | PASS then, but **stale** — needs fresh render post-55569cd |
| cavern_lake | battlemap_00093_ | PASS then, **stale** |
| tavern | battlemap_00094_ | PASS |
| temple_altar | battlemap_00095_ | PASS |
| ruined_castle | battlemap_00096_ | PASS |
| flooded_palace | battlemap_00097_ | PASS |
| cultist_fortress | battlemap_00098_ | PASS |
| broken_bridge | battlemap_00099_ | PASS |
| marble_palace | battlemap_00100_ | PASS |
| burning_quarter | battlemap_00115_ | PASS — plaza open, fountain free, street features clear, top edge flat, doors exterior |
| misty_swamp | battlemap_00102_ | PASS |
| goblin_gorge | battlemap_00103_ | PASS |
| forest_glade | battlemap_00104_ | PASS |

`python tools/render_scene.py tools/scenes/<name>.json` (needs ComfyUI). `--plan-only` builds plan/caption/preview without GPU — always do this first when scene JSON changes.

### Things NOT to do

- Don't special-case any of the 13 test scenes or goblin_cave to make them pass — every fix must generalise (pipeline/caption/architect). User forbids scene-specific hacks except legitimate scene-data rewording (like the masonry fix above).
- Don't add Claude/AI attribution to commits.
- Don't skip the C++ side — a fix only in `tools/` is half a fix.
- Don't claim "more top-down now" without checking the **top edge** of the PNG — that's where 3/4-view hides.
- Don't weaken any check/lint to make it pass.

---

## Night session log (autonomous batch — newest entries at the bottom)

Log format: one line per finished unit of work, each committed as it lands.

- batch opened: tree clean at `e596e66`, Ollama up (qwen3.8:27b), ComfyUI up (0.33.4). Queue: A) five planner stress tests; B) full 13-scene re-render; C) close-out sweep.
- A1 tavern DONE: 34 prop kinds (bar, stool, cask, hearth, stair...), layout building, 5 areas. One warning — LLM's own room prose said "flanked by" twice ('flank' is side-on); system worked. Caption: zero non-negated side-on hits; raw "in the wall" x12 are legitimate door wordings. Artifacts `output/_ollama_test/tavern/`.
- A2 harbour DONE (600s timeout first try, 900s retry ok): layout harbour, areas Moored Ship / Wet Quay / Timber Store / Quay, 25 floor-safe kinds. Zero warnings, caption clean. Artifacts `output/_ollama_test/harbour/`.
- A3 flooded_crypt DONE (233s): layout dungeon, 35 kinds (sarcophagus, stalactite → curated floor-safe phrase, skeleton...). Two warnings ('on the wall', 'vaulted'). GAP found: "stone niches are set high in the walls" slipped through because "in the wall(s)" was excluded to protect pipeline door wording.
- FIX `a047074`: doors now fill "an opening in the stonework", windows open through the wall, gaps "break that wall" (both ports + _phrases.json), and `in/into-the-wall(s)` joined the side-on list. Four committed users reworded; crypt now warns. All checks + parity green.
- HARDENING `51a92e8`: unknown style id now reports "not installed" instead of silent style-less build (own test guessed `misty_swamp`/`burning_quarter`).
- A4 swamp_shrine DONE (marsh_bog, 210s): 40 kinds, one warning ('hanging' in "thick hanging moss") — new hang words working. Caption otherwise clean.
- A5 burning_street DONE (city_streets, 217s): 42 kinds, one warning ('hangs' in "Smoking Crossroads says 'hangs'"); caption clean.
- ITEM A CLOSED: five plans built and verified; every warning was the LLM's own side-view prose being caught; two gaps fixed en route.
- B1 magic_hall PASS (battlemap_00092_): top edge flat, crystal dais, four platforms, bridges, chasm and library as planned.
- B2 cavern_lake PASS (battlemap_00093_): flat rock rim, lake south, causeway, spires, platform and cave mouth as planned.
- B3 tavern PASS (battlemap_00094_): one undivided hall, bar left, stair right, tables centre, door as marked; top edge flat.
- B4 temple_altar PASS (battlemap_00095_): one hall, aisle, altar top, pew quadrants, collapsed hole, doors as marked.
- B5 ruined_castle PASS (battlemap_00096_): courtyard with inner ring, tower spiral, well, colonnade, gates as marked.
- B6 flooded_palace PASS (battlemap_00097_): vestibule/stair/fountain/gilded doors, throne hall columns/dais, library, treasury — water reads as wet floors more than deep water (nuance, not structure).
- B7 cultist_fortress PASS (battlemap_00098_): six rooms as planned — entry rune, chapel pews, crypt pit, armoury, ritual hall, corridor doors.
- B8 broken_bridge PASS (battlemap_00099_): river, broken crossing, rope, trees/cart/tower west, terrace/pillars east.
- B9 marble_palace PASS (battlemap_00100_): gallery colonnade/statues, grand hall carpet/tables/double stair, banquet table, garden parterres/fountain.
- B10 burning_quarter PASS (battlemap_00101_): **VERDICT OVERTURNED by user** — fountain inside a burning building, quarter grew buildings not in plan. Root cause + fix in morning entries below; re-rendered as 00105 and judged clean there.
- B11 misty_swamp PASS (battlemap_00102_): causeway, water channels/pools, boulders, trees/reeds, sunken shrine ring.
- B12 goblin_gorge PASS (battlemap_00103_): gorge floor between rock bands, stream, tents, fire ring, platforms, scree, cave mouth; banners flat.
- B13 forest_glade PASS (battlemap_00104_): thicket ring, central stump, cut stumps/boulders, standing-stone row, stream with crossing.
- All thirteen scenes judged: thirteen passes (one later overturned), no re-renders needed at the time.
- Bare hang-family fix `bbd71f4`: "hanging/hangs/hang" now side-on; "nothing overhanging" stays legal via negation. Four more users reworded.

### Morning after the night session (user working directly)

- User overturned B10: fountain inside a building, invented buildings. Three caption causes fixed in both ports (`f37dc68`, `2b65991`): summary-vs-count reconciliation ("The only building in this picture is the Stone House…"), leftover-ground sentence, `two-storey/storey above/upper floor` → side-on.
- Re-render `battlemap_00105_` — fountain on open square, street/gate/waggon/gallery/rubble as planned, top edge flat. Some flame still inside the "still whole" stone house (summary framing).
- Full sweep green after fixes.
- User flagged: fountain still walled on 00105, and plaza too small. Scene replanned 48×40: burning houses become real building rooms (tenement, burnt-out house, warehouse — summary now plan-true), Fountain Square 16×16 with fountain + basin water + benches/bushes/trees/lamps, street flush, gatehouse wall+door gap. Plaza warning added to both ports (<15 cells).
- Renders 00106–00110 got fountain out of buildings and killed the perimeter, but stone stubs kept ringing the square and stall rows filled the street. Root cause: open-ground elements named `wall, fence, railing, kerb` inside negations — renderer drew the nouns. All purged from open-ground wording, gap sentence and open-site note (`55569cd`); on open site the note is dropped entirely. Render-tested next as 00111 (above).

### Night session summary

| Unit | Result |
|---|---|
| planner: tavern | 34 kinds, 1 warning ('flank'), caption clean |
| planner: harbour | 25 kinds, 0 warnings, caption clean |
| planner: flooded_crypt | 35 kinds, 2 warnings ('on the wall', 'vaulted') — GAP 1 slipped |
| planner: swamp_shrine | 40 kinds, 1 warning ('hanging') after GAP 2 fix |
| planner: burning_street | 42 kinds, 1 warning ('hangs'), caption clean |
| non-negated caption hits | ZERO on all five built captions |
| GAP 1 (fixed a047074) | `in/into the wall(s)` → side-on; pipeline door/window/gap reworded first |
| unknown style (fixed 51a92e8) | build_map names missing style instead of silent build |
| GAP 2 (fixed bbd71f4) | `hanging/hangs/hang` → side-on; "nothing overhanging" stays legal |
| renders | 13 scenes re-rendered — all PASS at the time, top edge flat, no people/text/grid |

---

## Previous handoff text (kept for context)

### The goal

Battle maps from a text description (human/agent/Ollama) must match that description structurally and be strictly top-down orthographic, zero tilt. Fix must land in both `tools/*.py` and `app/include/*.h`; verify with `python tools/check_caption_parity.py`.

### Architecture in one paragraph

A "map" is a grid of tiles (`tools/architect.py` / `app/include/map_architect.h`) built from a `spec` (rooms, layout, terrain zones, annotations, effects). The architect turns that into `zones` and `features`. `tools/ideogram_prompt.py` / `app/include/ideogram_caption.h` turn the finished map into a structured JSON caption for Ideogram 4.0 — almost all "looks nothing like the plan" bugs live in the caption's wording, not the pixel grid. `styles/*.json` supply per-genre wording; `styles/_base.json` supplies wording common to every map (viewpoint, forbidden-content suffix, border note).

### What's been fixed this session (grouped)

**Top-down enforcement (critical):** base aesthetics volumetric→flat orthographic; closing sentence now says top edge same straight-down view as bottom, no far wall/distance; `_SIDE_ON_WORDS` lint flags side-view phrasing; tavern's joists — viewpoint now says every floor *above* this one gone, no beam/joist/rafter/frame; decorative border wording reworded; `roof/ceiling` and `two-storey/storey above/upper floor` and `hanging/hangs/hang` and `in/into-the-wall(s)` joined the list (negation-aware `side_on_hit`/`SideOnHit`); prop-kind sanitiser `prop_kind_words()`/`PropKindWords()` rewords wall/overhead out of prop kinds and clutter; `style_warnings`/`MapWarnings` + `check_captions.py` final gate flush out shipped offenders; `natural_cave.json` cleaned.

**Structural fidelity:** sealed regions get a way in (`_ensure_a_way_in` counts only outside doors); wall component only "a building" if it encloses walkable floor (flood-fill); open-air layouts join outdoor rooms and open the field edge but preserve scene-placed walls; annotation label that names ground/wall actually paints it; caption opening states real geography, not just `scene_summary`; style materials trimmed when scene already describes layout; summary-vs-count reconciliation; leftover-ground sentence; wall-noun purge (see current state).

**Planner/tooling:** `place_in_field()`, `enclosed` field, richer prompts; app `StyleWarnings`; seed from `spec["seed"]` honored; `place_in_field` etc.; CMake no longer wipes `dist/output/`.

### Open issue that was the previous handoff's focus (now closed or carried above)

Goblin Cave's Ollama-invented prop kinds (`rusted_iron_sword_on_a_nail` etc.) baked side-view language into the kind string via the `a <kind>` fallback — lint did not scan prop kinds. Fixed by prop-kind sanitiser (see above). `natural_cave.json` cleaned and committed. Zone fragmentation 250–300 zones for 74×58 cavern is not a bug — strips merge into wall runs/open-ground blocks before caption budget. Rebuilding the app no longer wipes output.

Wording the new gate flushed out (`fbfe849`), rebuilding the app stops throwing saved maps away (`cfb4f5f`), and the remaining items above are what is left.

Written for whichever model picks this up next.
