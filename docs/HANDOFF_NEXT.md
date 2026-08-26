# Handoff: prompt-adherence work, current state

## Night session (autonomous batch — newest entries at the bottom)

Log format: one line per finished unit of work (planner test, render verdict,
fix), each committed as it lands. If work stops mid-item, the last line here
says exactly where.

- batch opened: tree clean at `e596e66`, Ollama up (qwen3.8:27b), ComfyUI up
  (0.33.4). Queue: A) five planner stress tests; B) full 13-scene re-render;
  C) close-out sweep.
- A1 tavern DONE: 34 prop kinds (bar, stool, cask, hearth, stair...), layout
  building, 5 areas. One warning — LLM's own room prose said "flanked by"
  twice ('flank' is a side-on word); system worked as intended. Caption scan:
  zero non-negated side-on hits; raw "in the wall" x12 are all the
  documented legitimate door wording ("a closed door filling this opening
  in the wall"). Artifacts output/_ollama_test/tavern/.
- A2 harbour DONE (600s timeout on first try, retry at 900s planned in
  157s): layout harbour, areas Moored Ship / Wet Quay / Timber Store / Quay,
  25 floor-safe kinds (bollard, mast, capstan, crate_stack...). Zero
  warnings, caption scan clean (one legitimate door wording hit only).
  Artifacts output/_ollama_test/harbour/.
- A3 flooded_crypt DONE (233s): layout dungeon, 35 kinds (sarcophagus,
  stalactite -> curated floor-safe phrase, skeleton...). Two warnings, both
  the LLM's own prose ('on the wall' in its summary, 'vaulted' in a room).
  FOUND A GAP: its "Candlelit Niche Chamber" wrote "stone niches are set
  high in the walls" and that passed SILENTLY - "in the wall(s)" was kept
  out of _SIDE_ON_WORDS to protect the pipeline's own door wording. Fixing
  next: reword the pipeline door/window/gap sentences out of that shape,
  then promote the phrase family into the lint in both ports.
- FIX COMMITTED (`a047074`): doors now fill "an opening in the stonework",
  windows open through the wall, building gaps "break that wall" (both
  ports + _phrases.json), and in/into-the-wall(s) joined the side-on list.
  The widened words at once caught four more committed users
  (sewer_tunnels rungs, magic_hall crystal threads, temple_altar and
  flooded_palace doors) - reworded. Re-planning the crypt spec now warns
  'in the walls' for Candlelit Niche Chamber. All checks + parity green.
- HARDENING (`51a92e8`): an unknown style id now reports "not installed"
  in problems instead of silently building a map with no style block -
  found because my own test guessed two style ids that don't exist
  (misty_swamp -> marsh_bog; burning_quarter is a scene, its style is
  city_streets). The app has no free-text style path, so tools-side only.
- A4 swamp_shrine DONE after style-id fix (marsh_bog, 210s): 40 kinds,
  ONE warning - "the render details says 'hanging'" - the promoted hang
  words catching live LLM prose ("thick hanging moss") that previously
  passed silent; helper sees it in background + element too. Caption
  otherwise clean. Artifacts output/_ollama_test/swamp_shrine/.
- A5 burning_street DONE after style-id fix (city_streets, 217s): 42
  kinds, ONE warning ("the Smoking Crossroads says 'hangs'", again the new
  words working); caption scan fully clean. Artifacts
  output/_ollama_test/burning_street/.
- ITEM A CLOSED: five plans built and verified; every warning that fired
  was the LLM's own side-view prose being caught (flank x2, vaulted,
  on-the-wall, hanging, hangs); two gaps found and fixed en route
  (in/into-the-wall promotion `a047074`, unknown-style guard `51a92e8`).
- B1 magic_hall PASS (battlemap_00092_): strictly top-down, top edge a flat
  wall band with no far-side recession; crystal dais, four platforms on
  bridges, south chasm and library corridor all match the plan; clean of
  figures, lettering and grid.
- B2 cavern_lake PASS (battlemap_00093_): flat rock rim all round, lake
  south with central causeway, floor spires as top-down pillars, platform
  and lit cave mouth where the plan puts them; clean.
- B3 tavern PASS (battlemap_00094_): one undivided hall as planned, bar
  left, stone stair right, overturned tables centre, door in the bottom
  band; top edge flat, no recession; clean.
- B4 temple_altar PASS (battlemap_00095_): one undivided hall, central
  aisle, altar on stepped platform top, four pew quadrants, collapsed hole
  bottom-centre, doors as marked; top edge flat; clean.
- B5 ruined_castle PASS (battlemap_00096_): courtyard with inner ruined
  ring, watch tower's spiral stair as a ring of wedge treads, central
  well, top colonnade, gates as marked; clean.
- B6 flooded_palace PASS (battlemap_00097_): vestibule/stair/fountain and
  the gilded doors flat in-plane, throne hall's column rows and dais,
  library shelves and table, treasury chest ring - all as planned; water
  reads as wet green floors more than deep water (adherence nuance, not
  structure); top edge flat; clean.

Written for whichever model picks this up next. Read [AGENTS.md](../AGENTS.md)
first — "How the renderer reads what you give it" is the accumulated rulebook
this whole effort has been building, and every fix below is recorded there
too, in more detail, with the reasoning.

## What the last session did (update; everything below this section is context)

The OPEN ISSUE at the bottom of this file was picked up and closed:

- **The prop-kind gap is fixed in both ports.** `prop_kind_words()` in
  `tools/ideogram_prompt.py` and `PropKindWords()` in
  `app/include/ideogram_caption.h` reword or cut anything wall/overhead-shaped
  out of a prop kind before it becomes caption text: "on the wall" clauses are
  cut ("faint_chalk_mark_on_the_wall" → "a faint chalk mark"), "in the wall" /
  "on a nail" get cut too (kept out of the shared `_SIDE_ON_WORDS` list on
  purpose — captions legitimately say a door fills an opening in the wall),
  stalactites/support beams/chandeliers are reworded into floor-safe forms.
  The same sanitising runs over clutter words. Verified against the actual
  goblin_cave map: zero wall/nail phrases left in its rebuilt caption.
- **Warnings now cover what the lint used to miss.** `style_warnings` (py) and
  `MapWarnings` (C++, called from both build paths in main.cpp) report prop
  kinds that had to be rewritten and any scene prose (summary/render_details/
  lighting/annotations/areas) asking for a side view. Curated phrase entries
  are exempt. The Goblin Cave scene text ("a large iron cauldron hangs over
  the flames") now draws a warning when planned again.
- **The curated data was audited by a new final gate**: `check_captions.py`
  scans every built background and element description for `_SIDE_ON_WORDS`
  (negation-aware). That flushed out three shipped offenders, all fixed:
  banner "hanging flat against the wall", lamp "a hanging lantern", torch "in
  an iron wall bracket", fireflies "hanging in the air" (mirrored in
  architect.py EFFECTS and the C++ literals), plus "its face" in the rock
  enclosure boundary wording and the organic-wall sentence.
- **natural_cave.json cleaned and committed** (see below for how it landed):
  materials no longer says "stalactites hanging overhead" / "elevation";
  stalagmite spires stand on the floor instead. Its `stalactite` prop got a
  curated top-down phrase in `_phrases.json` + C++ so it survives the gate.
- **Rebuilding the app no longer wipes dist output** — CMakeLists used to
  `rm -rf` the whole dist folder on every build, taking every map saved from
  the app with it. It now clears only the folders it ships and leaves
  `output/` alone. (The hard way: the user's goblin_cave render was lost to
  exactly this during this session's rebuild.)
- All four checks green (`check_scenes`, `check_captions`,
  `check_caption_parity`, `check_layouts`) after a fresh Release build.

Still open after this session:

1. **cavern_lake / magic_hall confirmation renders** were rendered at the end
   of the session (`output/scene_cavern_lake/battlemap_00090_.png`,
   `output/scene_magic_hall/battlemap_00091_.png`). Inspected against the
   top-edge rule and clean — no 3/4-view leak anywhere, structures match their
   plans. The user has not yet eyeballed them; that sign-off is theirs to give.
2. The Goblin Cave scene should still be re-planned through the app by the
   user (their original description). The tools-side equivalent has now been
   run for real: qwen3.8:27b planned the cave description through
   `MapPlanner.plan_map` into natural_cave/cavern, and exactly one warning
   fired — "the Central Campfire says 'suspended'" — against an annotation
   the model wrote as "a black iron cauldron suspended over the flames", the
   very prose this work predicts. No invented wall-mounted prop kinds this
   time; the caption scanned clean of side-on phrasing (see below). Run
   artifacts sit in `output/_ollama_test/`. A stand-in reconstruction of the
   map lost to the packaging wipe lives in `output/goblin_cave/` and
   `dist/DndBattlemapGenerator/output/goblin_cave/` — same style, layout, grid
   and terrain as the original; the room plan is the agent's own, since the
   Ollama rooms of the deleted map are gone with it.

Closed in the latest batch:

- **A bare mention of a roof or a ceiling is caught now** (was "Still open"
  item 3, from the live run's "coating the ceiling in soot"). Both words
  joined `_SIDE_ON_WORDS`, and every consumer — style_warnings' field and
  prose scans, check_scenes, and the C++ StyleWarnings/MapWarnings — judges
  by a negation-aware hit test (`side_on_hit` / `SideOnHit`), so the
  captions' own "no roof, no ceiling" passes while the Smoke Haze prose now
  warns at plan time and fails the caption gate. The widened lint flushed
  out 27 non-negated mentions across 13 styles and 7 scenes ("with the roof
  taken off", "roofless cottages", "flat rooftops"), all reworded in place.
- **A seed in the spec is honored now.** `compose()` falls back to
  `spec["seed"]` when no seed argument is passed, and `architect.build()`
  records the random seed it actually used instead of writing None, so any
  plan reproduces from its own meta.seed. The C++ app has no matching gap:
  `DesignSpec` carries no seed field and `PickSeed()` always passes one.
- The prop-name rule joined the AGENTS.md rulebook ("A prop's own name can
  carry the wall into the caption").
- The tools-side planner was exercised end-to-end against live Ollama
  (item 2 above): 26 prop kinds planned, all floor-safe; the one warning
  quoted there; negation-aware scan of the built caption found zero
  non-negated occurrences of on/in-the-wall, nail, hanging, stalactite,
  support beam, or "its face".

Closed while this session was running:

- **Zone fragmentation was a red herring**, as suspected. Zone counts by
  themselves mean nothing: a 74x58 cavern plan carries ~250-300 zones of
  which ~85% are one-cell-wide strips, a 36x30 carries 61 at the same ratio,
  and two different styles on the same seed and size produce *identical*
  geometry — the style has no say in shape, only in props and wording. The
  strips merge into wall runs and open-ground blocks before any element is
  built, so the caption stays inside its budget and the renders read as
  caves. Nothing to fix.

---

# Previous handoff text (context for everything above)



## The goal, as the user has stated it repeatedly

Battle maps generated from a text description (by a human, an agent, or
Ollama) must match that description structurally — no invented walls, no
invented rooms, no missing entrances — and must be **strictly top-down**,
orthographic, zero tilt. The user has said explicitly: any deviation from
straight-down view is a **critical** defect, ahead of everything else, because
the map has to be playable — a wall drawn as a face covers grid squares a
token has to stand on. See the memory file
`top-down-view-is-critical` if your harness has access to it; if not, treat
this paragraph as that rule.

The fix must always land in **both** `tools/*.py` (the Python pipeline used by
agents/Ollama-via-tools) **and** `app/include/*.h` (the C++ app). These are
two independent ports of the same logic and must never diverge. Verify with
`python tools/check_caption_parity.py` after every change — it builds the same
map both ways and diffs the captions. If it's not green, the fix isn't done.

## Architecture in one paragraph

A "map" is a grid of tiles (`tools/architect.py` / `app/include/map_architect.h`)
built from a `spec` (rooms, layout, terrain zones, annotations, effects). The
architect turns that into `zones` (rect regions of one tile kind) and
`features` (point props). `tools/ideogram_prompt.py` /
`app/include/ideogram_caption.h` turn the finished map into a structured JSON
caption for the Ideogram 4.0 image model — this is where almost all of the
"looks nothing like the plan" bugs actually live, because the caption's
wording, not the pixel grid, is what the diffusion model obeys. `styles/*.json`
supply per-genre wording (materials, ground, lighting, boundary, aesthetics)
that gets merged into the caption; `styles/_base.json` supplies wording common
to every map (viewpoint note, forbidden-content suffix, border note).

## What's been fixed this session (chronological, newest first in git log)

Run `git log --oneline` for the full list with one-line summaries; each commit
body explains the specific render that exposed the bug and why the fix works.
Grouped by theme:

**Top-down view enforcement (the critical one)**
- Base style aesthetics used to ask for "carefully shaded volumetric detail" —
  a direct request for sides/volume. Now asks for flat orthographic projection.
- The caption's closing sentence now states explicitly: every part of the
  picture is drawn from straight above, no side of anything visible, **and**
  the top edge of the picture shows the same straight-down view as the bottom
  edge — no far wall, no distance, no part of the map farther from the viewer
  than any other part. This last clause was added *after* the user pointed out
  a cave render (`battlemap_00072_`) still had a receding back wall in the top
  third — an earlier "it's better now" assessment from the previous session was
  wrong and got called out correctly. **If you're auditing renders, check the
  top edge of the image specifically — that's where the 3/4-view leak shows up
  every time.**
- `_SIDE_ON_WORDS` lint (in both `ideogram_prompt.py` and `ideogram_caption.h`)
  flags style fields and scene annotations/descriptions that describe
  something only visible from the side: "its face", "rising the whole
  length", "taller than a man", "hanging from", "on the wall", "to the roof",
  "standing upright", etc. Runs over styles (`check_captions.py`) and over
  scene files (`check_scenes.py`).
- **Known gap, not yet fixed**: this lint does NOT check `render_details` /
  `scene_summary` free text for *newly-generated* Ollama plans, and does NOT
  check **custom prop kind strings** an LLM invents on the fly. See "Open
  issue" below — this is likely the actual cause of the user's latest failed
  render.
- Tavern kept coming back with a ring of heavy timber bays round the walls —
  turned out to be the joists of the storey above, not a roof. Caption viewpoint
  note now says every floor *above* this one is gone too, not just the roof,
  and explicitly "no beam, no joist, no rafter, no timber frame anywhere."
- A decorative stone border was appearing round the image edge, inside the
  playable field — the border-note wording ("like the blank paper border of a
  printed battle map sheet") was read as an instruction to draw a border.
  Reworded to say the ground just stops in a straight line, no frame/kerb/
  border/edging of any kind.

**Structural / plan-vs-render fidelity**
- Sealed regions (rooms with no door) now get one cut automatically
  (`_ensure_a_way_in`), but only counts a door as "a way in" if it leads
  *outside* the region — doors between two interior chambers don't count.
- A wall component is only called "a building" in the caption if it actually
  encloses walkable floor (flood-fill test) — a cliff running along one side
  of a gorge was being described as "one single building," which the renderer
  then closed into a walled compound.
- Open-air layouts (`enclosure == "open"`, e.g. streets/plazas/wilderness):
  outdoor rooms are joined into one continuous walkable surface and the
  playable-field edge is opened, but wall/terrain the *scene itself* placed
  (e.g. gorge cliffs at the map edge) is now preserved rather than dissolved
  by the edge-opening pass.
- A named region (annotation) whose label says what it structurally is (e.g.
  "collapsed floor", "stone rib", "iron door") now gets that ground/wall
  actually painted under it in the grid, not just described in prose over
  whatever tile happened to be there.
- The caption's opening sentence now states the map's actual geography
  (derived from real room/annotation positions) instead of only the free-text
  `scene_summary`, so the strongest part of the caption doesn't contradict the
  rectangles below it.
- Style `materials`/`ground`/`lighting` text is trimmed or dropped when it
  would contradict a scene that already describes its own layout (was
  overriding real room descriptions with generic style flavor text).
- Symmetry: tried an automated check for "is this plan its own mirror image,"
  removed it — too many false positives (temple naves, single open rooms).
  Manually rewrote `goblin_gorge` scene to be asymmetric; this is a
  scene-authoring concern, not something the pipeline should auto-detect.

**Planner / agent tooling**
- `tools/planner.py`: added `place_in_field()` (fractional room placement),
  richer prompt language, `enclosed` field support so a planner can mark a
  room as open-air explicitly.
- App-side (`ollama_service.h`) got the same style-warning lint the tools
  side already had (`StyleWarnings` mirrors `style_warnings`).

## Verification tools (run these before trusting any change)

```
python tools/check_scenes.py          # builds/validates all 13 test scenes, structural checks
python tools/check_captions.py        # lints every style file for the side-on/hanging words etc.
python tools/check_caption_parity.py  # app vs tools caption diff — MUST be clean after any caption change
python tools/check_layouts.py         # generator sanity across layouts/sizes/seeds
```

All four are green as of the last commit (`9952cbf`).

## The 13 test scenes (`tools/scenes/*.json`)

These are the regression suite — real descriptions turned into scene files by
hand, each exercising a different structural challenge (multi-room dungeon,
open-air gorge, natural cave, flooded ruins, magic-hall bridges, etc). Full
source prompts are in `tools/scenes/prompts/`.

**Rendered and visually confirmed good** (by the user or by me inspecting the
PNG) as of the last render each got: tavern, temple_altar, ruined_castle,
flooded_palace, cultist_fortress, broken_bridge, marble_palace,
burning_quarter, misty_swamp, goblin_gorge, forest_glade, cavern_lake
(re-rendered after the "no far side" fix — **not yet re-confirmed by the
user**, last render was queued but its output hasn't been reviewed in this
conversation).

**Not yet rendered with the current pipeline**: magic_hall — was rendered
once earlier in the session (looked good structurally: octagonal room, four
platforms, bridges, pit) but that was *before* several of the caption fixes
above landed. Should be re-rendered and re-checked, particularly the top
edge for the 3/4-view leak.

Render with: `python tools/render_scene.py tools/scenes/<name>.json`
(needs ComfyUI running locally). `--plan-only` builds the plan/caption/preview
without spending GPU time — always do this first when scene JSON changes.

## OPEN ISSUE — the thing that needs to be picked up next

The user just tested the **actual app** (not the `tools/` scripts) end-to-end:
wrote a scene description ("Goblin Cave"), had Ollama plan it via the app's
in-app planner, rendered it, and got a result they call "полный отстой,
вообще на схему не похож" (total garbage, doesn't resemble the plan at all).

The rendered image (`goblin_cave/battlemap_...png`, shown to me but not saved
in this repo — ask the user to re-share or re-render) shows an oval/blob cave
shape with a strong 3/4-perspective feel around the edges, not a clean
rectangular top-down room layout.

I pulled the map JSON they generated
(`dist/DndBattlemapGenerator/output/goblin_cave/map.json`) and found concrete
evidence of two probable causes, **neither fixed yet**:

1. **The style `natural_cave.json` is untracked** (`git status` shows it as
   `??`) — it was created through the app's style editor, not part of my
   committed work, but it *does* already contain my "flat orthographic
   projection" phrasing in `aesthetics`, meaning it was authored/edited after
   pulling a recent build. It has NOT been run through `check_captions.py`
   (do that first — `python tools/check_captions.py` only scans styles it
   finds in `styles/`, so it should catch it, but confirm).

2. **Ollama invented prop kind strings that bake in side-view/wall-mounted
   language directly into the prop name**, e.g.:
   - `"rusted_iron_sword_on_a_nail"`
   - `"small_iron_hook_in_the_wall"`
   - `"faint_scratch_marks_on_the_wall"`
   - `"faint_chalk_mark_on_the_wall"`
   - `"faint_painted_symbol_on_the_wall"`
   - `"wooden_support_beam"`

   These become caption text like *"a rusted iron sword on a nail"* — which is
   exactly the class of thing `_SIDE_ON_WORDS` is supposed to catch, **except
   the lint only scans style fields and scene annotation/room text, not
   individual custom prop-kind strings the app turns into phrases via
   `normalize_prop`/`_PROP_WORDS` fallback ("a " + kind.replace("_"," "))**.
   This is almost certainly a real, previously-unknown gap. Trace it in
   `tools/ideogram_prompt.py` around where `phrases["props"].get(kind)` falls
   through to the `"a " + kind.replace("_", " ")` fallback, and the equivalent
   in `app/include/ideogram_caption.h` (`PropPhrase`). The lint needs to run
   over prop-kind strings too, or the fallback phrase-builder needs to strip/
   reject `_SIDE_ON_WORDS` substrings before they reach the caption.

3. Separately — worth checking but not yet confirmed as a bug — the map's
   `zones` array is extremely fragmented: hundreds of 1-cell-wide floor/wall/
   rubble strips rather than clean merged rectangles. This may just be normal
   for the `cavern` layout's organic rock-edge generation (irregular by
   design), or it may indicate the cavern generator is producing pathologically
   noisy geometry for this grid size (70×54, unusually large). Compare against
   `cavern_lake` scene's zones (clean, `underdark_cavern` style, similar
   layout) to see if `natural_cave`'s cavern generation is actually worse, or
   if this is a red herring.

**Suggested first steps for the next model:**
1. Read the user's original prompt (pasted in conversation) and the map.json
   (`dist/DndBattlemapGenerator/output/goblin_cave/map.json`) they attached.
2. Run `python tools/check_captions.py` and manually check whether
   `natural_cave.json`'s fields trip `_SIDE_ON_WORDS` — if the style file
   itself is fine, the bug is almost certainly the custom-prop-kind gap in #2
   above.
3. Fix the prop-kind lint gap in *both* `tools/ideogram_prompt.py` and
   `app/include/ideogram_caption.h` — this needs to happen for any prop kind
   reaching the caption, not just ones with a hand-written phrase in
   `styles/_phrases.json`.
4. Commit `styles/natural_cave.json` if it's meant to be a permanent style (ask
   the user, or infer from whether it's referenced in scene files / UI
   defaults) — right now it's dangling untracked and would be lost on a clean
   checkout.
5. Re-plan the same "Goblin Cave" description through the app after the fix
   and get the user to confirm the render before moving on.
6. Once that's closed, finish re-confirming cavern_lake (rendered, not yet
   reviewed) and magic_hall (stale render, needs a fresh one) against the
   current pipeline, per the regression-suite table above.

## Things NOT to do

- Don't special-case any of the 13 test scenes or the goblin_cave prompt to
  make them render correctly — every fix must be a pipeline/caption/architect
  fix that generalizes. The user has said this explicitly and repeatedly.
- Don't add Claude/AI attribution to any commit.
- Don't skip the C++ side. A fix only in `tools/` is half a fix.
- Don't trust "it looks more top-down now" without checking the top edge of
  the image specifically — that's the one place this class of bug hides.
