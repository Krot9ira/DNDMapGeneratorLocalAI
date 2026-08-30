# Assembling our map plans in Dungeondraft — implementation plan

Status: proposed, not started. Written 2026-08-30, for a fresh implementer.

This document is self-contained. It assumes no memory of the conversation that
produced it. Everything stated as fact below was verified on this machine or
read out of official documentation; where something is unverified it is marked
**UNKNOWN** and given a way to find out. Do not replace an UNKNOWN with a
guess.

---

## 1. Goal

This project already turns a scene description into a **map plan**: a JSON
grid of terrain zones, wall runs, doors, windows, props and effects. That part
works and is not being changed.

What we could not do was *make the art*. The previous attempt generated tile
sets with an image model; it is frozen on `feature/tiled-generation` and the
short version of why is that the model paints regions beautifully but cannot be
told which region is which material — water came back as mud, wall as a hedge.
Three attempts, three different scramblings.

So we stop making art and start arranging art that exists. The user owns 146
Dungeondraft asset packs. **Dungeondraft's map file is JSON.** We write it.

The finished feature:

1. Index every asset the user can use, and understand what each one *is* by
   looking at it with a vision model.
2. Take a map plan plus a style and emit a `.dungeondraft_map` the user opens
   and edits like any other map.
3. For **props only** — never terrain, never walls — generate what the library
   cannot supply, through Ideogram, cut the background out, and pack it into a
   custom asset pack.

---

## 2. Verified facts

Each of these was checked on this machine. The check is named so it can be
repeated.

### 2.1 The environment

| Thing | Value |
|---|---|
| Dungeondraft install | `D:\programs\dungeondraft\Dungeondraft` |
| Main data pack | `Dungeondraft.pck` — Godot 3.4.2, 4857 entries |
| User config | `C:\Users\kahas\AppData\Roaming\Dungeondraft\config.ini` |
| User asset packs | read `custom_assets_directory` from the config - it was `D:\programs\dungeondraft\assets`, then `F:\assets`, in one afternoon |
| Thumbnail cache | `%APPDATA%\Dungeondraft\.thumbnails` — 1947 files |
| Mods folder | `<install>\mods`, ships one example mod (`custom_snap`) |
| Asset pack template | `<install>\example_template.zip` |

`config.ini` is INI with JSON-ish array values. The keys that matter:

```ini
[Assets]
custom_assets_directory="D:\\programs\\dungeondraft\\assets"
active_asset_packs=[ "0Xoxtw2U", "2GkpDnbj", ... ]   # 110 ids
disable_default_assets=false
[Mods]
active_mods=[  ]
```

`disable_default_assets=false` means **the built-in assets are in play too**
and must be indexed alongside the packs.

### 2.2 The asset packs, and their real scale

**Every number here is a snapshot of a moving target.** Over one afternoon the
asset directory moved from `D:\programs\dungeondraft\assets` to `F:\assets`,
the pack count went 146 -> 8 -> 154 as packs were reinstalled, and the active
list in `config.ini` went from 110 ids to 133. **Read `custom_assets_directory`
and `active_asset_packs` from the config on every run, hard-code nothing, and
re-scan rather than trusting a cached count.**

At the last measurement:

| | |
|---|---|
| pack files on disk | 154 |
| unique pack ids | 133 |
| active in `config.ini` | 133 |
| matched to a file | 132 |
| enabled but absent | 1 (`bdd7mqo6`) - report, do not crash |
| textures in enabled packs | **264 599**, about 21 GiB |

| Category | Count |
|---|---|
| **objects** | **248 011** (12.1 GiB) |
| paths | 3 952 |
| patterns | 3 816 |
| terrain | 3 192 |
| portals | 2 098 |
| walls | 1 621 |
| tilesets | 864 |
| lights | 717 |
| roofs | 244 |
| caves | 52 |
| materials | 32 |
| *everything except objects* | *16 588 (9.2 GiB)* |

Three packs account for most of it: `FA_Objects_A_v3.54` alone holds 129 098
textures, `WFW Fantasy A 2.01` 44 124, `FA_Objects_B_v3.93` 41 402.

**File types: 249 587 `.webp`, 14 255 `.png`, 757 `.jpg`.** The documentation
says PNG; reality is overwhelmingly WebP. Pillow reads WebP, but anything that
assumes `.png` will silently index almost nothing.

**The md5 field in the PCK file table is all zeros** - Dungeondraft does not
fill it. Content identity has to be hashed by us, from the bytes.

**There is almost no duplication to exploit.** Of 248 011 object textures,
247 903 are unique by name and size: 108 duplicates, 0.04 %. Do not plan around
dedup saving the enrichment run - it will not.

### 2.3 A pack is a Godot 3 PCK, and it opens with sixty lines of Python

No third-party tool is needed to read packs. This works and was run:

```python
import struct, json

def read_pack(path):
    """Godot 3 .pck: magic, 4 version uints, 16 reserved uints, file count,
    then per file: path length, path, offset, size, 16-byte md5."""
    with open(path, "rb") as f:
        if f.read(4) != b"GDPC":
            raise ValueError("not a Godot pack")
        fmt, vmaj, vmin, vrev = struct.unpack("<4I", f.read(16))
        f.read(16 * 4)                                  # reserved
        (count,) = struct.unpack("<I", f.read(4))
        table = []
        for _ in range(count):
            (plen,) = struct.unpack("<I", f.read(4))
            path_ = f.read(plen).rstrip(b"\x00").decode("utf-8")
            offset, size = struct.unpack("<QQ", f.read(16))
            f.read(16)                                  # md5, unchecked
            table.append((path_, offset, size))
        return f, table       # read blobs by seeking to offset
```

Every pack carries `res://packs/<id>/pack.json`:

```json
{ "name": "Caeora Cave Assets", "id": "C7GVCedk", "version": "1",
  "author": "Caeora",
  "custom_color_overrides": { "enabled": false, "min_redness": 0.1,
                              "min_saturation": 0, "red_tolerance": 0.04 } }
```

Asset paths inside a pack are `res://packs/<id>/textures/<category>/...`.
Built-in assets are `res://textures/<category>/...` with **no** `packs/`
segment. Both forms appear in map files verbatim.

`Dungeondraft-GoPackager` and `DDTools` exist and do the same job; we do not
need them for reading. We may need a packer for step 5 — see §8.5.

### 2.4 The map file - read from three real maps

Read from `D:\programs\dungeondraft\maps\Ravine Bridge*.dungeondraft_map`.
`.dungeondraft_map` is Godot-flavoured JSON, and vectors and arrays are
**strings**: `Vector2( 960.533, 1052.2 )`, `PoolVector2Array( x1, y1, ... )`,
`PoolByteArray( ... )`, `PoolIntArray( ... )`.

Top level is two keys, `header` and `world`:

```
header
  creation_build      "1.1.0.0 newborn phoenix"
  creation_date       { year, month, day, weekday, dst, hour, minute, second }
  uses_default_assets bool
  asset_manifest      [ one entry per pack the map uses ]
  editor_state        camera, palettes, tag memory - cosmetic
world
  format              3        <- the version number to write
  width, height       25, 30   <- in grid cells
  next_node_id, next_prefab_id
  msi                 { offset_map_size, max_offset_distance, cell_size, seed }
  grid                { color, texture }
  wall_shadow, object_shadow, building_wear, trace_image_visible
  embedded            {}
  levels              { "0": { ... } }   <- keyed by string index
```

An `asset_manifest` entry:

```json
{ "name": "CH - Season Summer", "id": "3DDXdf2M", "version": "0,8",
  "author": "Crosshead", "keywords": null,
  "allow_3rd_party_mapping_software_to_read": false,
  "custom_color_overrides": { } }
```

**`allow_3rd_party_mapping_software_to_read` is a licence flag set by the pack
author, and it travels inside the map file itself.** Some packs set it to
false. Honour it - see section 9.

A level holds these keys:

```
label         str                  environment { baked_lighting, ambient_light }
layers        { "-400": "Below Ground", "-100": "Below Water",
                "100".."400": user layers, "700": "Above Walls",
                "900": "Above Roofs" }
terrain       { enabled, expand_slots, smooth_blending,
                texture_1 .. texture_8, splat, splat2 }
tiles         { cells, colors, lookup }
cave          { bitmap, entrance_bitmap, ground_color, wall_color, texture }
water         { disable_border }      shapes { polygons: [], walls: [] }
materials {}  patterns []  paths []   objects []  walls []  portals []
lights []     texts []     roofs { shade, shade_contrast, sun_direction, roofs }
```

**Terrain is a splatmap, and that is good news.** Up to eight texture slots and
two `PoolByteArray` splats. On a 25 x 30 map each splat is exactly 48 000 bytes
= 4 channels x 12 000 samples = 4 channels x (100 x 120), which is **four
samples per grid cell per axis**. `splat` carries the weights for textures 1-4,
`splat2` for textures 5-8; values run 0-255. A grid of blend weights is far
easier to write from our plan than the polygon soup this could have been.

**Tiles are a plain grid.** `tiles.cells` is a `PoolIntArray` of exactly
`width * height` ints in row order, `-1` meaning empty, otherwise an index into
`tiles.lookup`, a map from index string to texture path. This is the closest
thing in the format to our own plan grid.

**Caves are a bitmap.** `cave.bitmap` and `cave.entrance_bitmap` are
`PoolByteArray`s, 1584 bytes each on a 25 x 30 map. The layout is **UNKNOWN**:
all three sample maps have empty caves, so every byte is zero and the
dimensions cannot be inferred. 1584 does not factor cleanly against 25 x 30 at
1x, 2x or 4x. Do not guess - get a map with a cave drawn on it.

The object, pathway and pattern-shape entries are confirmed exactly, read out
of the example prefabs Dungeondraft ships at
`res://prefabs/Examples/*.dungeondraft_prefab` inside `Dungeondraft.pck`:

```json
{ "position": "Vector2( 960.533, 1052.2 )",
  "rotation": 1.832596,
  "scale": "Vector2( 1, 1 )",
  "mirror": false,
  "texture": "res://textures/objects/supplies/crates/fruit_box_05.png",
  "layer": 100,
  "shadow": false,
  "custom_color": "ff87a868" }
```

```json
{ "position": "Vector2( 1170.88, 1198.08 )", "rotation": 0,
  "scale": "Vector2( 1, 1 )",
  "edit_points": "PoolVector2Array( 11.0005, 2.1665, -56.6665, 1.33337 )",
  "smoothness": 1, "texture": "res://textures/paths/shadow_(flat).png",
  "width": 12.8, "layer": 200,
  "fade_in": true, "fade_out": false, "grow": false, "shrink": false,
  "loop": false }
```

```json
{ "position": "Vector2( 91.6581, 38.3228 )", "shape_rotation": 0,
  "scale": "Vector2( 1.08056, 1.08056 )",
  "points": "PoolVector2Array( 1799.76, 286.587, ... )",
  "layer": 300, "color": "36000000", "outline": false }
```

Facts to take from those:

- **`rotation` is radians.** 1.570796 is pi/2, 0.785398 is pi/4.
- `layer` is 100 for objects, 200 for pathways, 300 for pattern shapes, and the
  level's `layers` map names every legal value.
- `custom_color` is optional, ARGB hex.
- `edit_points` and `points` are **relative to `position`**, flat x,y pairs.
- Asset paths appear verbatim: `res://packs/<id>/textures/...` for pack assets,
  `res://textures/...` for built-ins.

### 2.5 The unit is 256

Dungeondraft treats **256 authored pixels as one grid square**. Confirmed
against the official template's real files:

| Asset | Size | Meaning |
|---|---|---|
| `objects/sample_barrel.png` | 202 × 194 | ~0.79 × 0.76 grid squares |
| `objects/sample_cauldron.png` | 161 × 154 | ~0.63 grid squares |
| `portals/sample_door.png` | 256 × 64 | exactly 1 grid wide |
| `terrain/sample_terrain.png` | 2048 × 2048 | 8 × 8, seamless |
| `walls/sample_wall.png` | 1024 × 62 | plus `sample_wall_end.png` 9 × 64 |
| `tilesets/tileset_simple.png` | 1024 × 1024 | 4 × 4 tiles |

**Objects are not padded to squares or powers of two.** A prop's pixel size
*is* its physical size. This is the sizing rule for every generated prop.

### 2.6 The custom asset pack layout

From `<install>\example_template.zip`, which is the official template. Real
files, not a wiki summary:

```
<PackName>/
  preview.png                              256x320
  data/
    default.dungeondraft_tags
    tilesets/<name>.dungeondraft_tileset
    walls/<name>.dungeondraft_wall
  textures/
    objects/  terrain/  materials/  tilesets/  walls/  paths/
    portals/  lights/  roofs/  caves/
    patterns/normal/  patterns/colorable/
```

`data/default.dungeondraft_tags` is exactly this shape:

```json
{ "tags": { "MyTag": ["textures/objects/sample_barrel.png",
                      "textures/objects/sample_cauldron.png"],
            "Colorable": ["textures/objects/sample_cauldron.png"] },
  "sets": { "Example Set": ["MyTag"] } }
```

`pack.json` is **not** in the template — it is written at packaging time.

### 2.7 Dungeondraft has a real mod API, which we are not using yet

Mods are a folder, a `.ddmod` JSON manifest and `.gd` GDScript files whose
`start()` Dungeondraft calls automatically. A real shipped manifest:

```json
{ "name": "Custom Snap Mod", "unique_id": "Lievven.Snappy_Mod",
  "version": "1.2.5", "author": "Lievven",
  "description": "...", "dd_version": "1.1.0.6" }
```

`Level` exposes `Walls`, `Objects`, `Portals`, `Terrain`, `Lights`,
`Pathways`, `Roofs`, `FloorShapes`, `PatternShapes` plus `Deserialize()`
("load instances of placed assets in prefab data format").

**We are not using this for v1.** Writing the file directly needs no plugin, no
Godot, and no running Dungeondraft, and it is testable from a script. Also
`WallTool` takes a polyline from the UI rather than coordinates, so the plugin
route has a question mark over the one thing we most need. Revisit as an
in-app "Import plan" button once the file path works.

---

## 3. What is still UNKNOWN

Reading three real maps answered most of the format. What remains:

**UNKNOWN-1 - walls, portals, lights, materials and `shapes.polygons`.** All
three sample maps are outdoor ravine scenes and every one of those lists is
empty, so the shape of their entries is unknown. Everything else about a level
is known.

*How to resolve:* one more hand-made map containing a wall run, a door, a
window, a light, a material area and a cave region. Twenty minutes in
Dungeondraft settles the rest of the format.

**UNKNOWN-2 - the cave bitmap layout.** 1584 bytes on a 25 x 30 map, every byte
zero. Needs a map with a cave actually drawn. This matters more than it looks:
in a cave scene the walls *are* the cave bitmap, not the `walls` list - which
is very likely how our own cave styles will have to be built.

**UNKNOWN-3 - are pack ids stable across machines?** Texture paths embed the id.
If a pack gets a different id on another install, maps we write are not
portable. The safe behaviour is to resolve every path against the local index
at write time, which costs nothing and removes the question.

**UNKNOWN-4 - what `DungeondraftConsole.exe` does.** It ships in the install
folder. If it renders or converts headlessly it changes step 6. Ten minutes of
work to find out, not a blocker.

## 4. Architecture

Four components. Build them in this order; each is useful alone.

```
  map plan (exists)                    asset packs on disk (146)
        |                                        |
        |                            [1] indexer  -> assets.db
        |                                        |
        |                            [2] enrichment (qwen3.8:27b, vision)
        |                                        |
        +----------> [3] matcher + assembler <---+
                             |
                             |  props with no match
                             v
                     [4] prop foundry (Ideogram -> cutout -> custom pack)
                             |
                             v
                     <name>.dungeondraft_map
```

### 4.1 Component 1 — the indexer

Reads `config.ini`, walks the asset directory, opens each pack with the PCK
reader in §2.3, and writes one row per asset.

Also indexes the built-ins out of `Dungeondraft.pck` when
`disable_default_assets` is false.

Extracts each texture's bytes to measure it and to make a thumbnail. **Do not
extract every full-size texture to disk** — 3.5 GiB of packs would become far
more. Read the blob, measure it, write a small thumbnail, discard the blob.

### 4.2 Component 2 — enrichment

`qwen3.8:27b` — already the project's model, already in `config.json`, already
used by `tools/ollama_client.py`. Ollama reports its capabilities as
`completion, vision, tools, thinking` with a 262 144 context, so it can look at
the thumbnail. Nothing new to install.

`tools/ollama_client.py` **already supports images**: `generate(...,
images=[...])` base64-encodes them, and `format=` takes a JSON schema, so the
answer comes back schema-constrained rather than parsed out of prose.

One pass per asset, cached by content hash forever. At ~19 200 objects and a
second or two each this is a **five to ten hour first run** and seconds
thereafter. Make it resumable, make it show progress, and let it be run for one
pack at a time.

### 4.3 Component 3 — matcher and assembler

Plan + style in, `.dungeondraft_map` out. Deterministic: same plan, same seed,
same index, same file. Without that nothing is checkable.

### 4.4 Component 4 — the prop foundry

**Props only.** If a terrain or a wall has no match the user is told, and picks
another or buys a pack. This restriction is the lesson of the frozen branch:
single objects on transparent backgrounds are the one thing this project has
already proved it can generate reliably.

---

## 5. Getting the database right — the part everything else rests on

**This is the critical path. If the index describes the assets badly, every map
built from it is wrong, and nothing downstream can recover.** A wrong
description does not raise an error. It does not fail a test. It sits in the
database, gets cached forever, and surfaces months later as a tavern furnished
with dungeon rubble. Treat this section as the specification, not as advice.

### 5.1 Never ask the model what you can measure

Split every field into one of two kinds and never mix them.

**Measured from pixels — the model never sees these questions.** Width,
height, grid footprint, whether there is alpha, how much of the image is
opaque, mean colour, dominant palette, content hash. These are facts. A model
asked "how big is this barrel" will guess, and its guess will be worse than
`width / 256.0` and impossible to distinguish from a real answer.

**Judged from the picture — only these go to the model.** What the object is,
what it is made of, what setting it belongs to, what style it reads as, and
whether it stands on the floor or hangs on a wall.

If a field can be computed, computing it is not an optimisation; it is the
difference between a fact and a plausible sentence.

### 5.2 What the model actually sees

The single most common way this kind of pass goes quietly wrong is that the
model is shown a bad picture and answers confidently about it.

- **Composite the alpha.** These are transparent PNGs. Rendered on black a dark
  iron brazier disappears; on white a pale statue does. Composite every
  thumbnail onto a **mid-grey** background, and record that choice, because it
  is part of the prompt whether or not it is written in words.
- **Pad to square, never stretch.** A 1024 × 62 wall squeezed into a square is
  a smear. Pad with the same grey.
- **Upscale the small ones.** Many objects are 100–200 px. Upscale to a
  consistent size — 512 is a reasonable target — with a decent filter. A model
  shown a 96-pixel thumbnail is guessing.
- **One asset per image.** Do not build contact sheets for the model. Contact
  sheets are for the *human* spot check (§5.6); a model asked about a grid of
  twenty objects will blur them together.

### 5.3 A controlled vocabulary, not free tags

Free-text tags drift and the drift is invisible until the matcher fails:
`medieval`, `mediaeval`, `middle ages`, `medieval fantasy` are four tags for
one idea and none of them match each other.

So: **the model picks from fixed lists** for `style_tags`, `setting_tags` and
`footprint`, enforced by the JSON schema's `enum`, and writes **one free-text
sentence** in `description` for everything the lists cannot hold. Both are
stored. The lists are what the matcher queries; the sentence is what a human
reads when a match looks wrong.

Draft the vocabularies *before* the run, from the categories the map plans
actually use — our `styles/*.json` and the props our planner emits are the
source. A taxonomy invented independently of what the matcher will ask for is
a taxonomy that will not answer.

Version the vocabulary alongside `prompt_version`. Changing it means the rows
written under the old one are stale, and you must be able to find them.

### 5.4 Let the model decline

Give every enrichment an explicit `confidence`, and allow `object_kind:
"unclear"`. A model with no way to say "I cannot tell" will invent, and an
invention is indistinguishable from an answer once it is in the database.

Low-confidence rows are not failures — they are a work queue. Never let a
low-confidence guess silently become the reason a prop was chosen.

### 5.5 248 000 objects will not fit through a blanket vision pass

This is the number that decides the design, so do the arithmetic before writing
any code. At one to two seconds per asset, **248 011 objects is 70 to 140 hours
of continuous inference.** That is not an overnight job, dedup will not rescue
it (section 2.2), and a run that long will be interrupted.

Everything that is *not* an object is a different story: **16 588 assets**
across terrain, walls, portals, paths, patterns, tilesets and lights - roughly
five to nine hours, one real overnight run. And those are precisely the assets
that decide whether a map looks like the style it claims: terrain and walls are
the look; a barrel is a detail.

So the enrichment is tiered, and the tiers are not an optimisation - they are
the design:

**Tier 0 - free, everything, no model.** Pack name, folder path, file name,
pack tags, plus the measured facts from section 5.1. Dungeondraft packs are
organised: paths like `textures/objects/supplies/crates/fruit_box_05.webp`
carry real signal, and Forgotten Adventures and WFW have disciplined trees.
This alone is a usable shortlist index, and it costs one scan.

**Tier 1 - vision, all 16 588 non-objects, run once.** Terrain, walls, portals,
paths, patterns, tilesets, lights. Overnight. These are what style matching
turns on.

**Tier 2 - vision, objects, on demand.** The matcher shortlists candidates out
of Tier 0, and only the shortlist gets looked at - then cached forever. The
working set converges on what this user's maps actually need instead of paying
for a warehouse they will never open. Run it as a background queue, seeded by
what the plans ask for.

**A measurement worth taking first:** FA's 129 098 textures are not 129 098
distinct objects - packs like that ship many recolours and variants of one
piece. Group object paths by folder plus base name with trailing variant
suffixes stripped, count the groups, and if the collapse is large, enrich one
representative per group and propagate. If it is not large, Tier 2 stands as
described. **Measure it; do not assume it either way.**

### 5.5b Measure quality before running any tier at scale

Whichever tier is running, the same discipline applies, because the output is
cached and a bad pass is paid for twice.

1. Take a **stratified sample of ~200 assets** - across categories, packs and
   sizes, deliberately including the awkward ones: very small, very wide,
   mostly transparent, near-monochrome.
2. Run the enrichment on those 200.
3. **A human reads all 200** against their thumbnails and marks each right,
   partly right or wrong. Two hundred is an hour and it is the cheapest hour in
   this project.
4. Fix the prompt, the thumbnail rendering or the vocabulary. Bump
   `prompt_version`. Repeat.
5. Only then start a tier at scale.

Keep the labelled 200 in the repository as a fixture, so that "I improved the
prompt" becomes a number instead of a feeling.

### 5.6 Checks that run after the full pass

Quiet failures need active looking-for:

- **Tag distribution.** If 60 % of assets carry one style tag, that tag is
  noise and the vocabulary needs work.
- **Empty and default answers.** Count rows with no semantic tags, with
  `unclear`, or with confidence under threshold. A sudden spike means something
  broke mid-run.
- **Duplicate descriptions.** Identical sentences across visually different
  assets mean the model stopped looking at the image.
- **Cross-check against context.** The pack name and folder path carry real
  signal: an asset in `Caeora Cave Assets/textures/objects/` described as
  "ship's rigging" is worth flagging. Record both the model's answer and the
  contextual expectation so disagreement is *findable*, and let vision win —
  but not silently.
- **Contact sheets for humans.** Generate pages of thumbnails with their
  descriptions underneath. A person scanning a page of forty spots a systematic
  failure in seconds that no aggregate would show.

### 5.7 Determinism and repeatability

- Temperature 0 and a fixed seed. This pass is not creative writing.
- `think=False` — the reasoning preamble costs time and leaks into output.
- Cache by `content_hash`, so the 20 duplicated packs and the many "Sharpened
  For Zoom" variants are described **once**, not five times.
- Never overwrite an enrichment row in place. Write a new row under a new
  `prompt_version` and keep the old one until the new one is trusted. You will
  want to compare, and you will not get a second chance to look at what the old
  prompt said.
- Resumable and interruptible: the run must survive a crash at asset 14 000 and
  pick up where it stopped. Per-asset try/except, and never let one bad image
  end the run.

### 5.8 Design the fields from the queries, not the other way round

Before writing the schema, write down the questions the matcher must answer:

> "a wooden barrel that stands on the floor, medieval, fits a tavern, roughly
> one grid square, in warm brown"

Every clause in that sentence is a field. Any field that no query will ever use
is a field that cost hours of model time for nothing, and any clause with no
field behind it is a query that will silently return rubbish. Do this on paper
first, with real examples taken from the props our planner actually emits.


## 6. Database schema

SQLite. Reuse `app/include/asset_db.h` and `tools/asset_library.py` — the
canonical-key identity scheme, the hashing and the schema versioning are sound
and already tested. Retarget the tables; keep the machinery.

```sql
CREATE TABLE packs (
  id            TEXT PRIMARY KEY,   -- "C7GVCedk"
  name          TEXT NOT NULL,
  author        TEXT,
  version       TEXT,
  file_path     TEXT NOT NULL,      -- absolute; the copy we indexed
  file_size     INTEGER NOT NULL,
  file_mtime    INTEGER NOT NULL,
  is_builtin    INTEGER DEFAULT 0,
  enabled       INTEGER DEFAULT 0,  -- from config.ini active_asset_packs
  duplicate_of  TEXT,               -- set when 2+ files share an id
  indexed_at    INTEGER,
  scan_version  INTEGER NOT NULL
);

CREATE TABLE assets (
  id            TEXT PRIMARY KEY,   -- sha256 of pack_id + res_path
  pack_id       TEXT NOT NULL REFERENCES packs(id),
  res_path      TEXT NOT NULL,      -- "res://packs/<id>/textures/objects/x.png"
  category      TEXT NOT NULL,      -- objects|terrain|walls|portals|paths|
                                    -- materials|tilesets|patterns|lights|
                                    -- roofs|caves
  subpath       TEXT,               -- "supplies/crates" — the pack's own tree
  file_name     TEXT NOT NULL,
  width         INTEGER NOT NULL,
  height        INTEGER NOT NULL,
  grid_w        REAL,               -- width / 256.0
  grid_h        REAL,
  has_alpha     INTEGER,
  alpha_coverage REAL,              -- fraction of opaque pixels
  mean_rgb      TEXT,               -- "#RRGGBB"
  palette       TEXT,               -- JSON array of dominant colours
  thumb_path    TEXT,
  pack_tags     TEXT,               -- JSON array from default.dungeondraft_tags
  pack_sets     TEXT,
  content_hash  TEXT NOT NULL,      -- sha256 of the image bytes
  UNIQUE (pack_id, res_path)
);

CREATE TABLE enrichment (
  content_hash  TEXT PRIMARY KEY,   -- keyed by CONTENT, so identical art
                                    -- across packs is described once
  description   TEXT NOT NULL,
  object_kind   TEXT,               -- "barrel", "oak tree", "iron brazier"
  semantic_tags TEXT NOT NULL,      -- JSON array
  style_tags    TEXT NOT NULL,      -- JSON array: medieval, dwarven, ruined...
  setting_tags  TEXT,               -- tavern, cave, dungeon, forest, ship
  dominant_hue  TEXT,
  footprint     TEXT,               -- floor|wall-mounted|ceiling|overhang
  confidence    REAL,
  model         TEXT NOT NULL,
  prompt_version INTEGER NOT NULL,
  created_at    INTEGER
);

CREATE TABLE generated_props (
  id            TEXT PRIMARY KEY,   -- sha256 of name + description + recipe
  name          TEXT NOT NULL,
  description   TEXT NOT NULL,
  style_id      TEXT,
  png_path      TEXT NOT NULL,
  grid_w        REAL, grid_h REAL,
  backend       TEXT, seed INTEGER,
  packed_into   TEXT,               -- our pack's id once packed
  created_at    INTEGER
);
```

Indexes on `assets(category)`, `assets(pack_id)`, `assets(content_hash)`, and
whatever the matcher ends up querying.

**Keying enrichment by `content_hash` rather than by asset id matters**: the
user has 20 duplicated packs and many "Sharpened For Zoom" variants of the same
art. Describing each copy separately would waste hours of the first run.

---

## 7. Contracts

### 6.1 Enrichment request

One image, one schema-constrained answer. Sketch:

```python
SCHEMA = {
  "type": "object",
  "properties": {
    "object_kind":   {"type": "string"},
    "description":   {"type": "string"},
    "semantic_tags": {"type": "array", "items": {"type": "string"}},
    "style_tags":    {"type": "array", "items": {"type": "string"}},
    "setting_tags":  {"type": "array", "items": {"type": "string"}},
    "footprint":     {"type": "string",
                      "enum": ["floor", "wall-mounted", "ceiling", "overhang"]},
    "confidence":    {"type": "number"}
  },
  "required": ["object_kind", "description", "semantic_tags",
               "style_tags", "footprint", "confidence"]
}
client.generate(prompt, images=[thumb], format=SCHEMA, think=False)
```

Give the model the filename and the pack's own tags as context alongside the
image — they are unreliable alone but useful together with the picture. Bump
`prompt_version` whenever the prompt changes so stale rows can be found.

### 6.2 Assembler input

The existing map plan JSON. Do not invent a new format; read what
`tools/agent_api.py build_map()` produces, which is what the whole project
already speaks.

### 6.3 Assembler output

A `.dungeondraft_map`, plus a sidecar report naming: how many plan elements
found an asset, which packs were used, which props had to be generated, and
what could not be satisfied at all. The report is how this gets checked
without a human squinting at a picture.

---

## 8. The work, in order

### Step 0 — recon (BLOCKING)

Get a hand-made sample map out of Dungeondraft with two terrains, a wall run, a
door, a window, several objects and a light. Read its JSON. Write down the real
schema for `terrain`, `materials`, `walls`, `portals`, `lights` and the header,
and settle UNKNOWN-1 and UNKNOWN-2.

*Done when:* `docs/dungeondraft_map_format.md` exists and describes every
top-level key with a real example.

### Step 1 — the indexer, no model

Read `config.ini`; walk the asset directory with `os.walk`; open every pack;
write `packs` and `assets`; extract thumbnails; index the built-ins.

*Done when:* the database holds ~24 000 assets across 109 enabled packs, the
counts in §2.2 are reproduced, `bdd7mqo6` is reported as missing rather than
crashing anything, and the 20 duplicate ids are recorded rather than silently
overwritten.

### Step 2 — enrichment

The vision pass, resumable, cached by `content_hash`, one pack at a time.

*Done when:* a query like "wooden barrel, medieval, tavern, on the floor"
returns sensible assets across several packs, and re-running the pass costs
nothing.

### Step 3 — objects only, end to end

Place only props from a plan onto an otherwise blank map. No terrain, no walls.

*This is the moment the whole idea is proved or killed.* Open the result in
Dungeondraft. If the objects land in the right places at the right sizes, the
rest is work; if they do not, stop and reconsider before building more.

*Done when:* a map opens in Dungeondraft with the plan's props in the right
squares, right rotations, sensible scales.

### Step 4 — terrain, materials, walls, portals

Depends entirely on what step 0 found. Walls first: our plan already carries
wall runs, and the frozen branch's one durable lesson is that a wall is a
region with an edge, not a fence with a centreline — so a run converts to a
polyline cleanly.

### Step 5 — the prop foundry

Ideogram generates, BiRefNet `lucida` cuts out, the result is scaled so that
its intended grid footprint × 256 gives its pixel size, then written into our
own pack folder with a `default.dungeondraft_tags`, a `preview.png` and a
`pack.json`, and packed.

Packing needs either `Dungeondraft-GoPackager` (a CLI binary, the pragmatic
choice) or our own Godot PCK writer (we already read the format; writing it is
maybe another eighty lines, and removes an external dependency). Decide when
you get there. Reading is proved; writing is not.

Reuse from the frozen branch: `build_ideogram` and `make_cutout` graph builders
from `tools/tile_backends.py`, `prop_prompt` from `tools/tilegen.py`, and
`tools/check_library.py` as the quality gate — a refusal card is grey where
real art is not, calibrated 10 for 10 on real samples. Ideogram does refuse
some prompts; the schema-shaped caption already cut that from two in three to
one in four.

*Done when:* a prop the packs do not have appears on the assembled map, at the
right size, with a clean edge.

### Step 6 — checks

Run the thirteen scenes in `tools/scenes/` through the assembler and report,
per scene, how many plan elements found an asset and how many were generated.
That is the regression suite for this project and it should stay that way.

---

## 9. Gotchas, each of which has already bitten

- **`pathlib.rglob` silently loses files.** It found 131 packs where `os.walk`
  found 146. Use `os.walk`.
- **Non-ASCII paths.** One asset folder has a Cyrillic name. Open with explicit
  encodings; do not assume the console can print a path.
- **20 duplicate pack ids.** Dedupe by id; record which files collided.
- **One enabled pack does not exist.** Report, continue.
- **`config.ini` is not standard INI** — the values are JSON arrays. A regex on
  `active_asset_packs=[...]` is more honest than pretending `configparser`
  understands it.
- **Rotation is radians, not degrees.**
- **`Vector2` and `PoolVector2Array` are strings** with their own spacing. Match
  the spacing Dungeondraft writes; do not assume it is cosmetic until proved.
- **Do not extract full textures to disk.** Read, measure, thumbnail, discard.
- **Never generate terrain or walls.** Props only. This is the whole point.
- **Assets are WebP, not PNG.** 249 587 webp against 14 255 png. Match on
  category and folder, never on extension.
- **The PCK md5 field is zeros.** Hash content yourself if you need identity.
- **The asset directory moves and the active list changes.** Both came from
  `config.ini` and both changed during a single afternoon. Re-read, never
  cache.
- **Nine enabled packs set `allow_3rd_party_mapping_software_to_read` to
  false** - among them several Caeora packs and `[NBS1]`/`[NBS2]`. The flag is
  set by the pack author, it travels in `pack.json` and in the map file's
  `asset_manifest`, and it is a licence term, not a hint. **Honour it: do not
  index, describe, thumbnail or reference the contents of a pack that says no.**
  Record that the pack was skipped and why, and tell the user which of their
  packs are unavailable to us and that this is the author's choice.
- **Never copy a third-party pack's art.** We reference assets by `res://` path
  and the map's `asset_manifest` names the packs. Only props we generate go
  into a pack of ours. State this before anything ships.

---

## 10. What to reuse, and what to leave alone

**Reuse:** `app/include/asset_db.h`, `app/include/sha256.h`,
`tools/asset_library.py`, `tools/ollama_client.py` (images and schema support
already there), `tools/check_library.py`, the Ideogram and cutout graph
builders in `tools/tile_backends.py`, `prop_prompt` in `tools/tilegen.py`, the
vendored SQLite and its CMake wiring.

**Do not bring across:** `tile_mask.h`, `tile_cut.h`, `tile_assembler.h`,
`tile_atlas.h`, `tile_library.h`, `gen_style_sheet.py`, `tilebake.py`,
`tilegen.py` beyond the prop prompt, `check_tiled.py`, `check_tilemasks.py`,
`check_seamless.py`, `check_refusals.py`, `normalize_library.py`, and every
`--tile*` verb in `app/src/main.cpp`.

**Do not touch:** the planner, the styles, the Ideogram caption path, or the
existing whole-scene render. They work.

### Branch

`main` never received the tiled machinery — only the Ideogram documentation and
the caption key-order fix, both of which we want. **Start the new branch from
`main`**; it is clean by construction and nothing has to be deleted from any
commit. `feature/tiled-generation` stays pushed, untouched, as the record of
what was tried.

---

## 11. House style

This repository has conventions. Match them.

- Comments explain **why**, not what. If a line encodes a decision, say what
  the alternative was and why it lost.
- Prefer a named constant with a comment over a magic number.
- Every tool is runnable alone from the command line and prints something a
  human can read.
- Checks report numbers, not pictures — a wrong asset is a quiet failure.
- **Never add `Co-Authored-By` or any AI attribution to a commit message.**
  Commit messages end with the prose.
