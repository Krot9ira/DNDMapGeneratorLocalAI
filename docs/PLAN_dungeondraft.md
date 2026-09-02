# Assembling our map plans in Dungeondraft — implementation plan

Status: **in progress.** Written 2026-08-30 as a proposal; revised 2026-09-01,
after steps 1 to 4 were built and the first maps came out of the assembler;
revised again 2026-09-02, after the first generated map was **opened in
Dungeondraft** (9.0) and the geometry was rebuilt from what that showed. The
feature is committed on `feature/dungeondraftintegration`.

**Start at section 9.** It says what is finished, what is half finished, what
has not been started, and what is a decision somebody has to take with
Dungeondraft open. Sections 2 to 8 are background and have not changed except
where marked.

This document is self-contained. It assumes no memory of the conversation that
produced it. Everything stated as fact below was verified on this machine or
read out of official documentation; where something is unverified it is marked
**UNKNOWN** and given a way to find out. Do not replace an UNKNOWN with a
guess. Where a number is quoted - assets indexed, doors placed, props matched -
it came from running the thing, and the command that produced it is named.

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
need them for reading. We may need a packer for step 5 — see section 10.5.

### 2.3b The built-in assets are not stored as images

`Dungeondraft.pck` holds 1 792 object textures, but not one of them is a
readable PNG. Under `res://textures/.../x.png.import` sits a Godot import stub:

```ini
[remap]
importer="texture"
type="StreamTexture"
path="res://.import/atlas_globe_01.png-7f0b1f4b6f6753f0a34d7558eede299f.stex"
[deps]
source_file="res://textures/objects/activities/administration/atlas_globe_01.png"
```

The pixels live in that `.stex`, which is a `GDST` header followed by a
**lossless WebP beginning at its `RIFF` marker**. So reading a built-in asset
is: read the stub, parse `path=`, find that entry in the same pack, seek to the
first `RIFF`, and hand the remainder to Pillow. Verified - `acid_border` comes
back as 2048 x 87 RGBA, which is its real size.

Anything that looks for `.png` bytes in the built-in pack finds nothing at all,
silently. This is the single most likely way for pass 1 to appear to work while
cataloguing an empty set.

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
false. Honour it - see section 10.

**Corrected 2026-09-02, against a 1.2.0.1 file written by the user's own
Dungeondraft.** Three of the shapes below were guessed from the outdoor maps
and were wrong in ways that validated perfectly and rendered as nothing:
`terrain` carries four texture slots and one `splat`, not eight and two;
`tiles.colors` is an array of ARGB strings **one per cell**, not an object; and
`water` carries a `tree` of polygons beside `disable_border`. All three are now
written out with examples in `docs/dungeondraft_map_format.md`, which is the
canonical description - prefer it over this section where they differ.

A level holds these keys:

```
label         str                  environment { baked_lighting, ambient_light }
layers        { "-400": "Below Ground", "-100": "Below Water",
                "100".."400": user layers, "700": "Above Walls",
                "900": "Above Roofs" }
terrain       { enabled, expand_slots, smooth_blending,
                texture_1 .. texture_4, splat }
tiles         { cells, colors, lookup }
cave          { bitmap, entrance_bitmap, ground_color, wall_color, texture }
water         { disable_border, tree }   shapes { polygons: [], walls: [] }
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


### 2.4b Walls, portals, lights, roofs and caves

Read from `D:\programs\dungeondraft\maps\testmap.dungeondraft_map`, a 48 x 27
map made by hand with exactly these things on it. This settles what the outdoor
maps could not.

**All geometry is in pixels at 256 px per grid cell, absolute, from the top
left.** A 48-cell-wide map is 12 288 px wide, and the wall below runs from
x=2304 to x=3328, which is cells 9 to 13.

**A wall** is a polyline with a texture:

```json
{ "points": "PoolVector2Array( 2304, 1536, 2304, 4352, 3328, 4352, 3328, 1536 )",
  "texture": "res://textures/walls/battlements.png",
  "color": "ff726e65", "loop": true, "type": 1, "joint": 1,
  "normalize_uv": true, "shadow": true, "node_id": "2",
  "portals": [ ... ] }
```

**`type` distinguishes a free-drawn wall from a building's own wall.** In the
test map `wall[2]` has `type: 0`, and its `points` are byte-for-byte the same
string as `shapes.polygons[0]` - the building's floor outline. The two
free-drawn walls are `type: 1`. So `type: 0` is the wall the building tool
creates around a floor shape, and `type: 1` is one somebody drew.

**Portals are nested inside their wall,** not in the level's `portals` list -
that list stayed empty even with a door and a window placed. A portal:

```json
{ "position": "Vector2( 2304, 3712 )", "rotation": 1.570796,
  "scale": "Vector2( 1, 1 )", "direction": "Vector2( 0, 1 )",
  "texture": "res://textures/portals/door_05.png",
  "radius": 128, "point_index": 0, "wall_id": "0",
  "wall_distance": 0.772727, "closed": true, "node_id": "7" }
```

`radius` 128 is half a cell - the door is one cell wide. `point_index` is which
segment of the wall polyline it sits on and `wall_distance` how far along that
segment, as a fraction. So a door is placed *along a wall*, not at a free
coordinate: build the wall first, then attach.

**A light:**

```json
{ "position": "Vector2( 2727.99, 2860.73 )", "rotation": 0, "range": 5,
  "intensity": 1, "color": "ffeccd8b",
  "texture": "res://textures/lights/fragments.png",
  "shadows": true, "node_id": "19" }
```

**A roof** lives in `roofs.roofs` and is a polyline with a width:

```json
{ "position": "Vector2( 0, 0 )", "rotation": 0, "scale": "Vector2( 1, 1 )",
  "points": "PoolVector2Array( 3328, 4224, 2304, 4224 )",
  "texture": "res://textures/roofs/diamond_slate_gray/tiles.png",
  "width": 128, "type": 0, "node_id": "14" }
```

**`materials`** is a dict keyed by layer number as a string
(`{ "-400": [ ... ] }`), not a list.

**`shapes`** holds `polygons` - a list of `PoolVector2Array` strings, the
building floor shapes - and `walls`, the **node_id of the wall that bounds each
polygon, as a decimal integer**. In the test map `shapes.walls` is `[22]` and
the `type: 0` wall's `node_id` is `"16"`, which is 22 in hex. So the two lists
are parallel, and the link between a floor and its wall is the node id -
written in hex on the wall and in decimal in `shapes.walls`. Match that, or a
building will come back without its walls.

**`node_id` is a lowercase hexadecimal counter held as a string.** Walls,
portals, lights, roofs and objects all carry one. In the test map the ids used
are `0, 2, 6, 7, 14, 15, 16, 19, 1a, 1b, 1c` - hex, so the highest is
`0x1c` = 28 - and `world.next_node_id` is `"1d"` = 29. The 233-node map reads
`"144"` = 324 against a highest id of `0x143` = 323. So the rule is:
**parse as hex, and write `next_node_id` as hex(max + 1)**. Reading them as
decimal appears to work until an `a` shows up.

**Terrain splat resolution is confirmed exactly.** On this 48 x 27 map the
splat is 82 944 bytes = 4 channels x 20 736 samples, and 4w x 4h = 192 x 108 =
20 736. Four samples per cell per axis, exactly, on both maps measured.

**The cave bitmap decodes, and it is confirmed.** One bit per sample, packed
eight to a byte, **least significant bit first**, row-major, over a grid of
**(4w + 3) x (4h + 3)** samples, rounded up to whole bytes:

```
bytes = ceil( (4*w + 3) * (4*h + 3) / 8 )
   25 x 30 map -> ceil(103 * 123 / 8) = 1584   (observed 1584)
   48 x 27 map -> ceil(195 * 111 / 8) = 2706   (observed 2706)
```

Decoded that way the test map's cave renders as a clean connected shape with
smooth edges, and the user confirmed it is what they painted. Decoded
most-significant-bit-first it is measurably more broken up (adjacency 0.81
against 0.90), so the bit order is settled by the picture and not by taste.

`cave` also carries `ground_color`, `wall_color`, `texture`
(`res://textures/caves/colorable/floor.png` here) and an `entrance_bitmap` of
the same size and layout.

**In a cave scene the walls are this bitmap, not the `walls` list.** That is
almost certainly how our own cave styles have to be built, so this formula is
on the critical path, not a curiosity.

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

Four of the user's maps and a purpose-made test map closed everything that was
open at the start. What remains is one small thing and one that costs nothing
to sidestep.

**UNKNOWN-1 - `wall.joint` and `normalize_uv`.** Both were `1` and `true` on
every wall measured, so their other values were never seen. Harmless: copy what
Dungeondraft writes and revisit only if a wall looks wrong.

**UNKNOWN-2 - none of consequence for steps 1 to 4.** The cave bitmap, the wall
and portal encoding, `wall.type`, `shapes.walls`, node ids and the terrain
splat are all settled and recorded in section 2.

**Closed, and why:**

- *Are pack ids stable across machines?* **Yes.** The id lives in `pack.json`
  *inside* the `.dungeondraft_pack` file, authored by the pack's creator, not
  assigned at install: the twenty packs present twice on this machine under
  different filenames all carry the same id in both copies. Paths embedding
  `res://packs/<id>/` are therefore portable. Resolving against the local index
  at write time is still worth doing, because it also catches a pack the user
  has not installed - but it is a safety net now, not a necessity.
- *What does `DungeondraftConsole.exe` do?* Nothing we can use. Run with
  `--help` it produces no output at all and does not exit; it had to be killed.
  It is not a headless CLI. Write the map file directly.

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

### 4.0 What exists on disk

All of it is built. Every tool runs alone from the command line and prints
something a human can read, as the house style requires.

| file | what it is | state |
|---|---|---|
| `tools/dungeondraft_pck.py` | Godot 3 PCK reader: opens a pack, lists and extracts textures, reads `pack.json` and the licence flag | done |
| `tools/dungeondraft_db.py` | SQLite schema, content hashing, image measurement, thumbnails | done |
| `tools/dungeondraft_indexer.py` | `scan`, `stats`, `validate` | done |
| `tools/dungeondraft_enrich.py` | the cataloguing pass over Ollama | done, never yet run at scale |
| `tools/dungeondraft_matcher.py` | plan element to asset | done, weak - see step 2 |
| `tools/dungeondraft_assembler.py` | map plan to `.dungeondraft_map` + report | done |
| `tools/check_dungeondraft.py` | the 13 scenes through the assembler | done |
| `tools/check_enrichment_quality.py` | 200-asset quality gate, numbers and a contact sheet | done |
| `tools/pipeline.py dungeondraft` | the command line entry | done |
| `tools/agent_api.py assemble_dungeondraft()` | the agent entry | done |
| app, **Dungeondraft** tab | export, scan, catalogue, quality check, validate | done |
| the prop foundry | section 4.4 | **not started** |

The app shells out to these tools with `python -u`; it does not reimplement any
of them. That is deliberate - the checks and the agent path run the same code
the buttons do, so a bug found from the command line is the bug the user has.

### 4.1 Component 1 — the indexer

Reads `config.ini`, walks the asset directory, opens each pack with the PCK
reader in section 2.3, and writes one row per asset.

Also indexes the built-ins out of `Dungeondraft.pck` when
`disable_default_assets` is false.

Extracts each texture's bytes to measure it and to make a thumbnail. **Do not
extract every full-size texture to disk** — 3.5 GiB of packs would become far
more. Read the blob, measure it, write a small thumbnail, discard the blob.

### 4.2 Component 2 — enrichment

**`gemma4:12b`, not the planner.** This paragraph used to name `qwen3.8:27b`
because that was already the project's model; section 5.7 then measured the
alternatives and that answer did not survive. The 27B lands 85 % on the CPU and
is ten to thirteen times slower than a model that fits in VRAM. Read 5.7 before
changing the cataloguer.

`tools/ollama_client.py` **already supports images**: `generate(...,
images=[...])` base64-encodes them, and `format=` takes a JSON schema, so the
answer comes back schema-constrained rather than parsed out of prose.

One pass per asset, **cached by content hash forever** - and the query that
selects work groups by that hash, so the same picture in four packs is one
question, not four. Resumable by construction: an asset that already has a row
at the current `prompt_version` is not selected again, so a cancelled pass
loses nothing but the item it was on.

Scale, measured on the five packs enabled today: 47 397 assets, 47 006 distinct
pictures. At the 2.0 s median of section 5.7 that is about **26 hours**. The
user's full library is five times that; section 5.7 puts it at just under six
days, which is why the quality gate in 5.9 exists.

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
- **Look at the thumbnails yourself before trusting any answer.** Write a
  contact sheet of forty and open it. This document told you to do that and the
  research behind it still went twenty measured calls before anyone did - at
  which point the "models cannot see" conclusion turned out to be "these
  objects are ambiguous from above", which is a completely different problem
  with a completely different fix. Ten seconds of looking.
- **One asset per image.** Do not build contact sheets for the model. Contact
  sheets are for the *human* spot check (section 5.10); a model asked about a grid of
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

**Half built, and the missing half has a deadline.** `footprint` is an `enum`
as required. `style_tags` and `setting_tags` are still free strings with a
length cap - the vocabularies were never drafted - and there is no
`vocabulary_version` column.

This is the one piece of unfinished work with a hard ordering constraint:
**settle it before pass 2, not after.** Enrichment is cached by content hash
and paid for in days; changing the schema afterwards means bumping
`prompt_version` and re-asking the model about a quarter of a million pictures.
Everything else in this document can be added later at the cost of an
afternoon. This cannot.

The lists themselves are not a design exercise: section 5.12 says to read them
off the queries, and the queries are already written - `styles/*.json` and the
`kind` values the planner emits, of which `tools/check_dungeondraft.py` will
print the exact working set.

### 5.4 Let the model decline

Give every enrichment an explicit `confidence`, and allow `object_kind:
"unclear"`. A model with no way to say "I cannot tell" will invent, and an
invention is indistinguishable from an answer once it is in the database.

Low-confidence rows are not failures — they are a work queue. Never let a
low-confidence guess silently become the reason a prop was chosen.

### 5.5 Two passes, because the user has to be able to start

The library is far too big to describe before the program is useful, so the
cataloguing is **two separate jobs the user starts themselves**, and the
program works after the first one.

**Pass 1 - the default assets.** Everything inside `Dungeondraft.pck`: about
1 792 objects and roughly 200 walls, terrains, portals, paths, tilesets and
lights. Two thousand assets. This is the set every Dungeondraft install has, it
is the same for every user, and it is small enough to finish in one sitting.
When it is done the program can already build a complete map out of stock
assets - which is exactly the state in which somebody can judge whether any of
this is worth continuing.

Because these assets are identical on every machine, **this catalogue can be
built once and shipped with the program.** A user who never runs anything still
gets a working map. Treat it as a data file we produce, not as work each user
repeats.

**Pass 2 - the user's own packs.** Everything in the enabled packs -
264 599 assets at the last count. This is the long one, it is per-user, it must
be resumable, and it should be startable per pack so somebody can catalogue the
three packs they care about tonight and the rest whenever.

The user interface follows the same split: two buttons, two progress bars, two
honest estimates in hours, and a clear statement of what works after each. Not
one opaque "indexing" that runs for days.

Within pass 2, order the work so the useful part lands first: everything that
is not an object (about 16 600 assets - terrain, walls, portals, paths,
patterns, tilesets, lights) before the 248 011 objects, because terrain and
walls are what makes a map look like its style, and a barrel is a detail.

### 5.6 Validate, then re-catalogue only what is broken

A shipped catalogue goes stale silently. Dungeondraft updates its built-in
assets between versions - textures get added, removed, redrawn under the same
name - and the catalogue we ship keeps confidently describing art that is no
longer there. The same happens to a user's packs every time one is updated.
Nothing errors. Maps just quietly start choosing worse assets, or referencing
a `res://` path that no longer resolves.

So the catalogue needs a **Validate** button, and validation must be cheap
enough that pressing it is never a decision: it reads pixels and hashes, and
calls no model at all.

**What Validate compares.** The catalogue against what is actually installed
now:

- **New** - an asset present in the pack with no row in the catalogue.
- **Gone** - a row whose `res://` path is no longer in the pack. These are the
  dangerous ones: a map written against them will not open cleanly.
- **Changed** - a row whose path still exists but whose image content hash
  differs. The art was redrawn; the description may now be a lie.
- **Resized** - dimensions changed, so the grid footprint in the row is wrong.
- **Unenriched** - a row with no enrichment, or enrichment written under an
  older `prompt_version` or an older vocabulary version.
- **Suspect** - enrichment that exists but is weak: `object_kind` of
  `unclear`, confidence below threshold, empty tag lists, or a description
  identical to another asset's (section 5.10 explains why that last one matters).

**What it reports.** Counts per category and per pack, and a written verdict in
plain language: *"1 792 default assets, 1 786 unchanged, 4 new, 2 redrawn, 0
missing. 6 need cataloguing, about two minutes."* Not a bare number, and not a
red cross with no explanation.

**Re-catalogue** then acts on exactly that list - the new, the changed, the
unenriched and the suspect - and nothing else. It is the same job as a pass,
pointed at a delta instead of at everything, and it obeys the same rules:
resumable, per-asset commits, failures recorded as rows.

Two properties make this work in practice:

- **Content hash, not file size or date.** A pack rebuilt with no real changes
  would otherwise invalidate everything in it. Hash the decoded image, so
  identical art recompressed is still identical art.
- **Never delete a row on the strength of an absence.** Mark it `gone` and keep
  it. A pack the user has temporarily disabled must not cost them the hours
  spent cataloguing it, and a pack re-enabled next week should light up again
  instead of starting over.

Validate belongs on both jobs. For the default assets it is the answer to
"we shipped this catalogue, is it still true on this Dungeondraft version"; for
the user's packs it is the answer to "I updated a pack, what do I owe".

**Built, in two tiers** - `python tools/dungeondraft_indexer.py validate`, and
the Validate button. The tiers matter, because the version of this that reads
every texture on every press is one nobody presses:

1. **Every pack, from its file.** Size and mtime against what the index
   recorded, plus the active list re-read from `config.ini`. New, gone,
   redrawn, newly enabled, newly disabled, unreadable, and the packs whose
   author disallows third-party reading. Costs a `stat` per pack.
2. **Every asset, but only inside a pack whose file changed.** New, gone,
   redrawn and resized, by decoded-image content hash exactly as above. An
   untouched pack file cannot contain a redrawn texture, so hashing a quarter
   of a million of them to prove it would be an expensive way to learn nothing.

Then the enrichment counts: unenriched, written under an older
`prompt_version`, and orphaned - a description whose picture no pack contains
any more. Exit code 0 when the index matches the packs on disk, 1 when there is
work to do, which is what the app's Validate button reads.

Two things in the list above are **not built yet** and are step 2b's remaining
work: **Suspect** (weak enrichment - `unclear`, low confidence, empty tags,
duplicate descriptions) is not detected, and **Re-catalogue** does not exist as
a verb - the delta is reported, but acting on it means re-running a pass, which
re-does everything unenriched rather than exactly the list. The `state = 'gone'`
marking is in the schema and honoured by every query; the scan does not yet set
it.

One correction learnt the hard way: compare a pack against **the file the index
actually read**, recorded in `packs.file_path`, not against whichever copy
`os.walk` reaches first. Twenty pack ids exist twice on this machine under
different filenames, and the naive version reported all twenty as redrawn on
every single run - a check that cries wolf twenty times is a check nobody
reads.

### 5.7 Which model to catalogue with, measured

Measured on this machine - an RTX 3080 Ti with 12 GB - through Ollama, on real
built-in object textures at 384 px, warm model, first call discarded, context
4 096, schema bounded as described below.

| model | size | placement | median | bad JSON | echoes | with colour+material | 248 011 objects |
|---|---|---|---|---|---|---|---|
| `qwen3.8:27b` (ctx 262 144) | 34 GB | 85 % CPU | 20.1 s | - | - | - | 58 d |
| `qwen3.8:27b` (ctx 4 096) | 18 GB | 52 % CPU | 14.0 s | - | - | - | 40 d |
| `qwen2.5vl:7b` | 5.5 GB | 100 % GPU | **1.08 s** | 0 | **11 / 14** | 7 / 14 | 3.1 d |
| `ornith-1.5:9b` | 6.6 GB | 100 % GPU | 1.51 s | 0 | 3 / 30 | 15 / 30 | 4.3 d |
| **`gemma4:12b`** | 7.6 GB | 100 % GPU | 2.00 s | 0 | **4 / 30** | **19 / 30** | 5.8 d |

Two things decide this, and neither is raw intelligence.

**A model that fits in VRAM is in a different class.** `qwen3.8:27b` at its
default 262 144 context needs 34 GB and lands 85 % on the CPU. Dropped to
4 096 - all a one-image question needs - it needs 18 GB, reaches 52 % GPU and
gets 30 % faster for free. The three small models sit entirely on the card and
are **ten to thirteen times faster than the 27B**. That is the difference
between four days and forty for the same library.

**Speed without looking is worthless.** `qwen2.5vl:7b` is the fastest by a
distance, and it is the wrong choice: it restated the file name as the answer
in **eleven of fourteen** cases. It was not cataloguing, it was reformatting
the filename. `ornith-1.5:9b` and `gemma4:12b` echo three or four times in
thirty and describe what is actually in the picture.

**Recommendation: `gemma4:12b` to catalogue, `qwen3.8:27b` to plan.** Gemma is
the slowest of the three small ones and still finishes the whole library in
under six days, and it earns that: it produced a usable description with both
colour and material for nineteen of thirty against fifteen, and read a grave
marker as a *stone sarcophagus*, an egg as a *monster egg* and a floor cloth as
a *discarded cloth* - answers the filename does not contain. Cataloguing is
paid once and read forever, so buy the better description.

`ornith-1.5:9b` is the reasonable second choice at 30 % faster, and is what to
switch to if six days is too long. Both are far better than the fast one.

**Set `num_ctx` per job, not globally.** The planner wants a huge window; the
cataloguer wants 4 096. They may be the same model, and if the cataloguer
inherits the planner's window it spills onto the CPU and runs ten times slower.

### 5.8 The blind test, and what it proved about context

Before adopting any of the above, one experiment is worth more than all the
timings. Run the enrichment twice on the same assets: once with the file name
and folder in the prompt, once with **only the image**.

That was done here, and the result changed the design.

With the name, both models looked excellent - `wall_torch`, `fireplace`,
`cauldron`, `statue`, `fern`, confidence 0.9 to 0.95. Blind, both fell apart:
the 27B called a globe a *pond*, a broken pillar a *wooden plank*, a bundle of
herbs a *torch*; the 7B called a floor cloth a *dog*, a blood stain a *heart*,
herbs a *sword*.

**The first reading of that was wrong, and it is worth recording why.** It
looks like proof that the models were reading the filename and not the image.
They were not. Looking at the thumbnails - which nobody had done until then,
in a document that tells you to - the pictures are clean and legible, and the
objects are simply **ambiguous from directly overhead at eighty pixels**. A
fireplace seen from above really is a stone rectangle. A broken pillar lying
down really does look like a plank. A red stain really is a red blob. A human
given only those pictures would answer no better.

Three conclusions follow, and they are the design:

1. **The file name and folder are legitimate context, not cheating.** Strip
   them and the task becomes unanswerable, for a model or a person. Keep them,
   and keep the pack name too.
2. **But echoing must be prevented.** The 7B returned `pillar_broken_11` as
   `object_kind` - the filename, digits and all, restated. That row adds
   nothing the indexer did not already have, while looking like an answer.
   Require the name to be normalised: **no digits, no underscores, and not
   equal to the file's stem.** Reject and retry once when it is; if it echoes
   again, mark it low confidence.
3. **The description must add what the name cannot.** Material, colour, state,
   whether it reads as top-down, whether it is lit. That is the part only the
   image can supply, and it is what the matcher will actually rank on.

**Design the human check around this.** The two-hundred-asset sample in section
5.9 must include assets whose filenames are unhelpful (`Rock 367B`, `prop_04`)
and assets whose filenames are *misleading*, because those are the only cases
that reveal whether the model is looking. Everything else grades the filename.

**Bound every field in the schema, or a tenth of the run comes back broken.**
This cost an hour to find and is the single most useful practical detail here.

With an unbounded schema - `{"type": "array", "items": {"type": "string"}}` -
`qwen2.5vl:7b` returned **ten unparseable answers out of fourteen**,
`Unterminated string`. Grammar-constrained generation must emit valid JSON, so
when the token budget runs out mid-array the output is not recoverable: it is
truncated JSON, not a model that answered badly. Raising `num_predict` alone
does not fix it, because the model simply writes longer tag lists.

Adding `maxItems` and `maxLength` to every field fixed it completely and made
it **faster**:

| schema | unparseable | median |
|---|---|---|
| unbounded, `num_predict` 400 | 10 / 14 | 1.79 s |
| bounded, `num_predict` 1500 | **0 / 14** | **1.08 s** |

The model stops writing sooner because it is not allowed to ramble, so the cap
is never reached. Concretely:

```python
"object_kind":   {"type": "string", "maxLength": 40},
"description":   {"type": "string", "maxLength": 140},
"semantic_tags": {"type": "array", "maxItems": 5,
                  "items": {"type": "string", "maxLength": 24}},
"style_tags":    {"type": "array", "maxItems": 3,
                  "items": {"type": "string", "maxLength": 24}},
```

At 1.08 s the whole library - 248 011 objects - is **about three days in one
stream**, which is the difference between this feature existing and not.

**And normalise in code, not in the prompt.** Told plainly to use lower case,
no digits and no underscores, and never to repeat the file name, the model
still returned `wall_torch`, `pillar_broken`, `sail_jib`, `bush_flower`. Strip
underscores and digits after the fact and compare against the file stem
yourself. A prompt is a request; a post-processing step is a guarantee.

### 5.9 Measure quality before running anything at scale

Whichever pass is running, the same discipline applies, because the output
is cached and a bad pass is paid for twice.

1. Take a **stratified sample of ~200 assets** - across categories, packs and
   sizes, deliberately including the awkward ones: very small, very wide,
   mostly transparent, near-monochrome.
2. Run the enrichment on those 200.
3. **A human reads all 200** against their thumbnails and marks each right,
   partly right or wrong. Two hundred is an hour and it is the cheapest hour in
   this project.
4. Fix the prompt, the thumbnail rendering or the vocabulary. Bump
   `prompt_version`. Repeat.
5. Only then start a pass at scale.

Keep the labelled 200 in the repository as a fixture, so that "I improved the
prompt" becomes a number instead of a feeling.

**Built as `tools/check_enrichment_quality.py`**, and as the *first* button in
the Dungeondraft tab's tagging section, above the three that start a real pass.
It draws the stratified sample, runs the enrichment, writes a contact sheet,
and - the part that makes it a gate rather than a gallery - prints the numbers:
how many answers came back at all, how many were the file name reworded, how
many said `unclear`, how many had a description under four words or confidence
below 0.5, the mean confidence, and the footprint spread. Over a quarter
echoing and it says so in words and tells you to change the model.

Steps 3 and 4 above are still a human's job, and step 5's fixture does not
exist: the labelled 200 are not kept, so a prompt change cannot yet be scored
against the previous one. That is the remaining work here, and it is worth
doing before the prompt is touched, not after.

### 5.10 Checks that run after a pass

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

**None of this exists yet.** The contact sheet does, and the echo and `unclear`
counts do, but only over the 200-asset sample in 5.9 - they run *before* a pass
to choose a model, not *after* one to catch a run that went wrong halfway. The
tag distribution, the duplicate-description check and the context cross-check
have no code at all. Write them when the first real pass has finished and there
is something to point them at; running a pass with nothing watching it is how a
quarter of a million cached mistakes happen.

### 5.11 Determinism and repeatability

- Temperature 0 and a fixed seed. This pass is not creative writing.
- `think=False` — the reasoning preamble costs time and leaks into output.
- Cache by `content_hash`, so the 20 duplicated packs and the many "Sharpened
  For Zoom" variants are described **once**, not five times.
- Never overwrite an enrichment row in place. Write a new row under a new
  `prompt_version` and keep the old one until the new one is trusted. You will
  want to compare, and you will not get a second chance to look at what the old
  prompt said.
- **Interruptible and resumable is a hard requirement, not a nicety.** Pass 2
  runs for days on a machine somebody also uses for other things. It must
  survive Ctrl-C, a crash at asset 140 000, a reboot, Ollama being restarted
  and the asset folder being reorganised mid-run - all of which happened during
  the research for this document.

  Concretely: commit each result to the database as it arrives rather than
  batching, so stopping loses at most one asset. Derive the work queue by
  asking the database what is missing rather than by keeping a cursor, so
  resuming is just running it again. Wrap every asset in its own try/except and
  record the failure as a row, so one unreadable image cannot end a run and so
  the failures can be retried on their own later. Let it be stopped and started
  per pack. Print progress with a real estimate of time remaining, and on
  resume say how much is already done.

### 5.12 Design the fields from the queries, not the other way round

Before writing the schema, write down the questions the matcher must answer:

> "a wooden barrel that stands on the floor, medieval, fits a tavern, roughly
> one grid square, in warm brown"

Every clause in that sentence is a field. Any field that no query will ever use
is a field that cost hours of model time for nothing, and any clause with no
field behind it is a query that will silently return rubbish. Do this on paper
first, with real examples taken from the props our planner actually emits.


## 6. Models the program needs, and getting them

The program depends on models the user may or may not have. Today that
dependency is a line in `config.json` naming `qwen3.8:27b`, and if it is absent
or misspelled the failure surfaces as a confusing error in the middle of a job.
With cataloguing added there are now two model roles, and the program should
handle both properly rather than assume.

**Two roles, named separately in the config:**

- the **planner** - text, plans a scene; today `qwen3.8:27b`
- the **cataloguer** - vision, describes assets; a small model that fits
  entirely in VRAM. Measured recommendation: **`gemma4:12b`**, with
  `ornith-1.5:9b` as the faster second choice. Section 5.7 has the numbers.

**Context size belongs to the job, not to the model.** The planner needs a huge
window; the cataloguer needs about 4 096 and is 30 % faster for having it. Send
`num_ctx` per request rather than leaving one global default, or the
cataloguer inherits the planner's 262 144 and spills onto the CPU.

**In the settings, per role: a dropdown, not a text field.** It lists the
models already installed in Ollama, read from `GET /api/tags`, so a user who
already has what they want simply picks it - nothing is downloaded and nothing
is decided for them. Beside the list, the models we recommend for that role,
each with its size on disk and a **Download** button that pulls it into Ollama
through `POST /api/pull`, which streams progress and can be shown as a real
progress bar rather than a spinner.

`GET /api/show` reports a model's capabilities - `qwen3.8:27b` returns
`completion, vision, tools, thinking`. **Use it.** The cataloguer dropdown must
refuse a model without `vision`, and say why, rather than letting somebody
start a four-day job with a model that cannot see. That check costs one request
and prevents the worst failure this feature can have.

Rules that make this honest rather than pushy:

- **Never download anything without being asked.** A button the user presses,
  never a silent pull on first run. These are multi-gigabyte files on somebody
  else's disk and possibly somebody else's metered connection.
- **State the size before the download, not during.**
- If the user has already named a model, leave it alone. The dropdown exists to
  help someone who has not chosen, not to override someone who has.
- Show what is missing plainly: if the configured model is not installed, say
  so on the settings page with the button next to it, instead of failing when a
  job starts.
- Downloads are resumable - Ollama handles that - so a cancelled pull is not a
  wasted one. Say so.
- A model can be removed too. Offer it, ask once, and never remove a model the
  config still points at without saying what that will break.

## 7. Database schema

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
  content_hash  TEXT NOT NULL,      -- sha256 of the DECODED image, so that a
                                    -- pack merely recompressed does not read
                                    -- as changed
  state         TEXT DEFAULT 'ok',  -- ok | gone | changed - never delete a row
                                    -- for a pack that is only disabled
  last_seen_at  INTEGER,
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

## 8. Contracts

### 8.1 Enrichment request

One image, one schema-constrained answer. This is the version that was
measured, not a sketch - **every field is bounded**, and section 5.7 explains
why an unbounded one returns ten broken answers in fourteen.

```python
SCHEMA = {
  "type": "object",
  "properties": {
    "object_kind":   {"type": "string", "maxLength": 40},
    "description":   {"type": "string", "maxLength": 140},
    "semantic_tags": {"type": "array", "maxItems": 5,
                      "items": {"type": "string", "maxLength": 24}},
    "style_tags":    {"type": "array", "maxItems": 3,
                      "items": {"type": "string", "maxLength": 24}},
    "setting_tags":  {"type": "array", "maxItems": 3,
                      "items": {"type": "string", "maxLength": 24}},
    "footprint":     {"type": "string",
                      "enum": ["floor", "wall-mounted", "ceiling", "overhang"]},
    "confidence":    {"type": "number"},
  },
  "required": ["object_kind", "description", "semantic_tags",
               "style_tags", "footprint", "confidence"],
}

client.generate(prompt, system=SYSTEM, format=SCHEMA, images=[thumb],
                temperature=0.0, think=False,
                num_predict=1500,   # never reached once the schema is bounded
                num_ctx=4096)       # per job - not the planner's window
```

The system prompt that produced the measured results:

```
You catalogue art assets for a tabletop battlemap editor. You are shown one
asset on flat grey, drawn as seen from directly overhead. The file name and
folder are given as a hint from the artist; use them, but say what you can see
that they do not say.
object_kind: two or three plain words, lower case, no digits, no underscores.
Never repeat the file name.
description: one sentence under twenty words, naming colour and material.
If the picture is too small or ambiguous to tell, use object_kind 'unclear'
and a low confidence rather than inventing.
```

and the user message carries the context that makes the task answerable at all:

```
Folder: supplies/crates
File name: fruit_box_05
Size: 202x194 px (0.79 x 0.76 grid squares)

Catalogue this asset.
```

Give the model the pack name and the pack's own tags too. They are unreliable
alone and useful together with the picture.

**Then normalise the answer in code.** Told plainly not to, the models still
return `wall_torch`, `pillar_broken`, `sail_jib`. Strip underscores and digits,
lower-case, and compare against the file stem yourself; a prompt is a request,
post-processing is a guarantee. Store the raw answer as well, so a change of
mind about normalising does not mean running the pass again.

Bump `prompt_version` whenever the prompt, the schema or the thumbnail
rendering changes, so stale rows can be found. All three are part of the
question being asked.

This is the schema as implemented, and it is **not yet what section 5.3 asks
for**: only `footprint` is an `enum`, while `style_tags` and `setting_tags` are
still free strings. The measurements in 5.7 and 5.8 were taken with this
schema, so the numbers stand; the vocabulary work in 5.3 changes what the model
is allowed to answer, and has to happen before a pass caches a quarter of a
million free-text answers.

### 8.2 Assembler input

The existing map plan JSON. Do not invent a new format; read what
`tools/agent_api.py build_map()` produces, which is what the whole project
already speaks.

### 8.3 Assembler output

A `.dungeondraft_map`, plus a sidecar report naming: how many plan elements
found an asset, which packs were used, which props had to be generated, and
what could not be satisfied at all. The report is how this gets checked
without a human squinting at a picture.

**The sidecar is written beside the map with the suffix replaced, not
appended**: `crypt.dungeondraft_map` and `crypt.report.json`. Both the app and
`check_dungeondraft.py` read it, so the keys are a contract - do not rename one
without the other. What it contains today:

```json
{
  "title": "...", "style": "gothic_crypt",
  "grid": {"cols": 40, "rows": 30},
  "packs_referenced": ["WFWFA2A"], "packs_referenced_count": 1,
  "props_matched_by_description": 0,   // the catalogue answered
  "props_matched_by_name": 57,         // only the file name matched - a guess
  "walls_placed": 29, "portals_placed": 5,
  "objects_placed": 57, "lights_placed": 8,
  "doors_unattached": 0,               // no wall within reach; dropped
  "matched_props":   [{"kind": "altar", "matched_asset": "res://...",
                       "pack_id": "...", "x": 10, "y": 21, "scale": 1.0,
                       "match_quality": "named"}],
  "unmatched_props": [{"kind": "banner", "x": 4, "y": 9}],
  "unattached_doors": []
}
```

`props_matched_by_name` is the number that tells you whether cataloguing has
happened. On an uncatalogued library it is the whole count, and every one of
those is a filename guess. `unmatched_props` is the list the prop foundry
exists to serve - do not let anything turn it into zero by inventing a match.

---

## 9. The work, in order

| step | what it is | state |
|---|---|---|
| 0 | recon | **done** - the format is read and written down; only the cave write-back is untested |
| 1 | the indexer | **done** - 47 397 assets from 5 enabled packs and the built-ins |
| 1b | model settings | **half done** - the vision check exists, the download UI does not |
| 2 | cataloguing | **built, effectively never run** - 2 of 47 398 assets have a description; fix the vocabulary (5.3) *before* running it |
| 2b | validate and re-catalogue | **validate done**, re-catalogue and suspect-detection not |
| 3 | objects only, end to end | **done and seen** - a map was opened in Dungeondraft on 2026-09-02; see 9.0 |
| 4 | terrain, tiles, walls, portals, lights | **done, caves included**; 9.7 is decided |
| 5 | the prop foundry | **built** - `tools/dungeondraft_foundry.py`, Ideogram render plus rembg cutout |
| 6 | checks | **done** - 13 scenes, plus a quality gate for cataloguing |
| 9.8 | use all 32 cores where it helps | **done where it was worth it** - the indexer is threaded (`scan --threads`); the cataloguer was measured at x1.04 and left alone |

Read 9.7 before touching walls, 9.8 before optimising anything, and 9.0 before
believing any of the rest.

### 9.0 Done, on 2026-09-02, and it was worth doing first

**A generated map was opened in Dungeondraft.** This section used to say nobody
had, and that everything under it had been checked by reading the JSON and
validating it against section 2 - which catches a malformed file, a duplicate
node id and a wrong array length, and catches nothing at all about whether the
map *looks* right.

It did not look right, and none of it showed up in validation:

- **Walls were disconnected black hairlines.** Two causes. The wall texture was
  being picked from the `*_end` cap sprites, which Dungeondraft draws at the
  tip of a run; and each wall rectangle was converted on its own, so a
  horizontal run and the vertical run it turns into sat on centrelines half a
  cell apart, leaving a hole at every corner and a stub past every end.
- **Water was a grid of flat blue slabs.** One polygon per rectangle, each
  drawing its own border, with `blend_distance` set to 32 - which is in cells,
  not pixels, so the shore blend covered the whole body.
- **No floor anywhere.** `tiles.colors` was written as `{}`; it is an array of
  one colour per cell, and without it the tile layer does not draw at all.

The fix was to stop translating rectangles and build the geometry from the
rasterised plan: wall tiles chained into polylines through their centres,
doorway tiles woven into the run so the portal cuts a continuous wall, and one
traced polygon per body of water. See the commit "Rebuild Dungeondraft walls,
water and floors from the tile grid".

**The lesson is the one this section was written to force.** Validation checks
that a file is well formed. Only Dungeondraft checks that it is right. Open the
map after any change to the assembler.

### Step 0 - recon

**Done**, and section 2 records it. `docs/dungeondraft_map_format.md` describes
every top-level key with a real example and ships with the release. Section 3
lists what is still unknown, and the answer is: nothing that steps 1 to 4 need.

One half of the original done-criterion is still outstanding, and it belongs to
the cave branch of step 4: the cave bitmap has been **read** and confirmed
against the map the user painted, but never **written** by us and opened again.
Reading a format correctly does not prove you can write it - the bit order was
settled by looking at a decoded picture, and only a round trip proves the
encoder agrees with the decoder.

### Step 1 - the indexer, no model

**Done.** `python tools/dungeondraft_indexer.py scan` reads `config.ini` for
`custom_assets_directory` and `active_asset_packs`, walks with `os.walk`, opens
every pack with the PCK reader, measures every image, writes a thumbnail, and
indexes the built-ins out of `Dungeondraft.pck`. Duplicate ids are recorded
rather than overwritten; an enabled pack that is not on disk is reported and
does not stop the scan; WebP is indexed like PNG.

Packs whose author set `allow_3rd_party_mapping_software_to_read` to false are
skipped, the pack row is kept so the user can be told why, and **no texture of
theirs is read, hashed, thumbnailed or referenced**. Ten such packs are
installed here. Validate lists them under SKIPPED every run, deliberately: the
user should be able to see which of their packs are unavailable to us and that
it is the author's choice, not a bug.

What it produced here: 134 packs, 5 enabled, 47 397 assets, 47 006 distinct
pictures, 44 788 of them objects. `data/assets.db` is 55 MB and the thumbnails
are 737 MB - both machine-local, both now in `.gitignore`, and neither ships.

*Still open:* the scan indexes textures only for **enabled** packs. That is a
sensible default and it is why the numbers above are 47 397 rather than the
264 599 in section 2.2, but `--all-packs` has had far less use. Also, the scan
does not mark a vanished asset `state = 'gone'`; it simply stops seeing it.

### Step 1b - model settings

**Half done.** The cataloguer is a separate config key (`dungeondraft.vision_model`,
default `gemma4:12b`) with its own dropdown on the Settings page listing what
Ollama has, and `num_ctx` is sent per request as 4 096 rather than inherited
from the planner. `check_vision_capability()` asks `GET /api/show` and a pass
refuses to start on a model without `vision`, with the reason on the log.

**Not done:** the recommended-models list, the sizes, and the **Download**
button over `POST /api/pull` with a real progress bar. Today a user without
`gemma4:12b` is told what is wrong and must go and pull it themselves. Section
6 has the rules that make that UI honest; they still apply.

The refusal is also in the wrong place to be kind: it happens when the job
starts, not when the model is chosen in the dropdown. Moving the check into the
dropdown is small and worth doing.

### Step 2 - cataloguing, in two passes the user starts

**Built and never run at scale.** `python tools/dungeondraft_enrich.py --scope
stock|custom|all`, and three buttons. Work is selected by content hash, so the
same picture in four packs is one question; an asset already described at the
current `prompt_version` is never selected again, which is what makes a pass
resumable and a second pass free.

**Every prop on every map today is matched by file name**, because nothing has
a description yet. That is the single biggest gap in the feature and it is not
a code gap - somebody has to press the button and wait.

The order to do it in:

0. **Settle the controlled vocabulary first.** Section 5.3 explains why this
   one cannot wait: the schema is what the answers are cached under, and
   changing it after pass 2 means paying for pass 2 twice.
1. **Then the quality gate** - the button, or `python
   tools/check_enrichment_quality.py --sample 200`. Section 5.9 says why, and
   it now prints the numbers that decide: how often the model handed the file
   name back, how often it said `unclear`, mean confidence. Over a quarter
   echoing means the model is not looking at the pictures; change it before
   cataloguing anything.
2. **Pass 1, the default assets** - 1 996 of them, about an hour. After this
   the program can furnish a map from stock assets alone on any machine.
3. **Pass 2, the user's own packs** - 45 401 at the current enabled set, about
   a day; five times that for the full library.

The numbers above come from `python tools/dungeondraft_indexer.py stats`. Run
it rather than trusting them; the enabled set changes.

*Not done*, three things, in the order they are worth doing:

- Pass 1's result is not **shipped as a data file**. It is identical on every
  install and no user should pay for it twice - that was the whole argument for
  splitting the passes, and the split exists while the shipping does not. What
  ships today is an empty `data/` folder.
- **Per-pack cataloguing is not reachable from the app.** The tool takes
  `--pack <id>`; the buttons offer only stock, custom and all. Section 5.5
  wanted somebody to be able to catalogue the three packs they care about
  tonight and the rest whenever, and that is still a command line away.
- **No progress bar**, only log lines. The tool prints a rate and an ETA every
  ten assets and the app shows them, which is honest but is not the "two
  progress bars, two honest estimates in hours" of section 5.5. For a job of
  this length that is a real difference.

Within pass 2 the work is already ordered non-objects first, as 5.5 asks.

### Step 2b - validate and re-catalogue

**Validate is done**, in the two tiers described in 5.6, with the asset-level
comparison inside changed packs. Verified by corrupting a copy of the database
and watching it name the redrawn, resized, gone and new assets correctly.

**Re-catalogue is not done**, nor is suspect-detection. See 5.6 for exactly
what is missing.

### Step 3 - objects only, end to end

**Done in code.** Props from a plan land on the map at their plan coordinates,
`(x + w/2) * 256` for the centre, rotation in radians.

Scale is not always 1. Half the object library is naturally under half a grid
square and that is deliberate, so small art is left alone; but art more than
1.5x bigger than the slot the plan gave it is scaled down to fit, because a
14-square statue dropped on a one-square feature swallows the room. That
tolerance is a constant, `PROP_OVERSIZE_TOLERANCE`, and it is a guess that
wants a human eye on it in Dungeondraft.

**Unproven.** See 9.0. Nobody has opened one.

### Step 4 - terrain, tiles, walls, portals, lights

**Done except caves.** Tiles are one int per cell in row order; terrain writes
both splat byte arrays at four samples per cell per axis; walls are polylines
in absolute pixels; portals are attached to them by `point_index` and
`wall_distance`; lights are placed for any prop whose kind reads as a flame.
Node ids are unique and `world.next_node_id` is above the highest.

Two things were wrong in the first version and are worth knowing about, because
both produced a file that validated perfectly and would have looked broken:

- **Not one door was ever placed.** The test asked whether the door's centre
  was nearer than half a cell to the wall line, and a door cell's centre is
  always *exactly* half a cell from it. Thirteen scenes, zero portals, no
  error. Doors are now attached by projecting onto the nearest wall segment,
  which also fixed the other half of it: only the top edge of a room was
  checked, though the comment claimed all four.
- **Free-standing walls were drawn corner to corner**, so a wall zone became a
  diagonal across the room. They are now the centre line of the zone's long
  axis.

The planner leaves a gap in the wall where a door goes, so the nearest point on
a wall is often its own end. A portal parked on an endpoint hangs half off the
wall, so the wall is extended through the opening and the door cuts it - which
is also how a person draws it in Dungeondraft.

**Caves are written.** The bitmap is one bit per sample, packed eight to a
byte, least significant bit first, over a `(4w + 3) x (4h + 3)` grid - the
three extra samples per axis straddle the map edge, 1.5 of them on each side.
It is filled from the rasterised floor of the plan, so a cave scene lays no
floor tiles and lets the cave layer carry the ground.

A cave scene still emits ordinary walls as well as the bitmap, and whether it
should is the remaining open question in this step. `cavern_lake` comes out
with its lake as two clean polygons and its stalagmites as one-cell wall boxes,
which reads correctly; the outer boundary wall may not want to be there at all.
Open one and decide.

### 9.7 Decided: wall zones win, and neither source is a rectangle any more

**Settled 2026-09-02 with Dungeondraft open.** Option 1 below - wall zones win,
room outlines are not drawn as walls - is what shipped, and `shapes.polygons`
is left empty rather than being given floor outlines with no wall id. Every
wall is `type: 1`.

What the question missed is that *both* sources were the wrong shape. A plan's
rectangles are a compressed decomposition of a tile grid, so translating either
of them a rectangle at a time is what put two lines half a cell apart in the
first place. Walls are now traced from the wall **tiles**: a one-cell-thick run
becomes a centreline through the cell centres, so a corner is one point on one
polyline and cannot be half a cell out; a solid mass of rock becomes its
outline instead. The rest of the section is kept as the reasoning that was
available before the file was opened.

Walls are built from two sources that disagree by half a cell:

- **`areas`** - room rectangles - each becomes a closed `type: 0` wall and the
  matching entry in `shapes.polygons` / `shapes.walls`, which is what makes it
  a building floor.
- **`zones` of kind `wall`** - what the planner actually rasterised - each
  becomes an open `type: 1` wall.

In the crypt scene the reliquary's outline runs at y = 512 and the wall zone
that *is* its north wall runs at y = 384. Two parallel walls, half a square
apart, all the way round every room. In the JSON this is invisible; on screen
it will not be.

The three ways out, none of which should be chosen from a JSON dump:

1. **Wall zones win.** Drop the room outlines as walls, keep the polygons as
   floor shapes. Risk: `shapes.walls` wants a wall node id for every polygon,
   and nothing we have read says what Dungeondraft does when there is not one.
   Cheap to find out - write one map both ways and open both.
2. **Room outlines win.** Drop wall zones that run within a cell of a room
   edge, keep the rest. Risk: a zone is not the same length as the room edge it
   overlaps - the crypt's north wall runs a cell past the reliquary at each end
   - so dropping it whole loses real wall.
3. **Snap.** Move each wall zone's line onto the room edge it is within a cell
   of, and merge duplicates. Most work, best result, and only worth it once
   somebody has confirmed 1 and 2 are wrong.

### 9.8 Using the whole machine, where it actually helps

Everything written so far runs on one core. This machine has **32**. The
question is not whether to parallelise but *what* - and the answer is not the
part that takes days.

Every number below was measured on this machine, and each measurement says
what it ran on so it can be rebuilt in a dozen lines: a fixed list of real
assets, a warm start, and a fresh thumbnail directory per run - `save_thumbnail`
returns early when the file exists, so reusing one makes every run after the
first look free and the first parallel figure came out 40 % too good. Do not
take these on trust once the code has changed. Take them again.

#### The indexer: threads, and roughly ten times faster

240 textures out of `WFW Fantasy A 2.01`, each one extracted from the pack,
decoded, hashed, measured and thumbnailed - the whole per-asset pipeline:

| | time | rate | speed-up |
|---|---|---|---|
| serial | 9.63 s | 24.9 assets/s | - |
| threads x4 | 2.54 s | 94.6 assets/s | x3.8 |
| threads x8 | 1.39 s | 172.6 assets/s | x6.9 |
| threads x16 | 1.11 s | 216.8 assets/s | x8.7 |
| threads x32 | 0.94 s | 255.1 assets/s | **x10.2** |
| processes x8 | 5.55 s | 43.2 assets/s | x1.7 |
| processes x16 | 4.15 s | 57.8 assets/s | x2.3 |

**Threads, not processes**, and the reason matters because it is the opposite
of the usual Python advice. Almost none of this work is Python: Pillow's WebP
decode, its LANCZOS resize and its WebP encode, `hashlib.sha256` over the
decoded bytes, and numpy's alpha and mean statistics all release the GIL, so
threads run genuinely in parallel. Processes have to pay Windows `spawn` - a
fresh interpreter, re-imported Pillow and numpy - and have to reopen the pack
per item because a `PckReader` cannot be pickled. They lose four-fold to
threads and win only 2x against doing nothing.

What that is worth: the current 47 397 assets take about **32 minutes** serially
and about **3**. The user's full 264 599 would be three hours against eighteen
minutes. The scan is the one job in this feature that is pure CPU, and it is
the one job the user re-runs whenever a pack changes.

Two things make it safe:

- **`PckReader.read_bytes` opens its own file handle per call**, so one reader
  can be shared across threads without a lock. This was not designed for
  threading; it is true by luck, and it is worth a comment in the reader so
  nobody "optimises" it into a shared handle.
- **The SQLite writes stay on one thread.** The connection is not shared across
  threads, and `save_thumbnail` writes into hash-sharded directories with
  `mkdir(exist_ok=True)`, which is safe concurrently. Structure it as a pool
  that decodes and measures, and one consumer that upserts - and collect
  results in submission order, not completion order, or section 5.11's
  determinism promise quietly stops being true.

Per texture within a pack, not per pack. Packs differ in size by two orders of
magnitude, so a pack-level pool would leave 31 cores waiting on the largest
one, and it would make the progress line meaningless.

#### The cataloguer: no, and it is worth knowing why

Sixteen real assets through `gemma4:12b`, warm model, first call discarded:

| | time for 16 | each | speed-up |
|---|---|---|---|
| one at a time | 32.8 s | 2.05 s | - |
| 2 in flight | 31.5 s | 1.97 s | x1.04 |
| 4 in flight | 31.7 s | 1.98 s | x1.03 |

**Nothing.** The 2.05 s also confirms section 5.7's measured 2.00 s median from
a different direction, so the measurement is sound and the answer is real: one
request already saturates the GPU, and Ollama serialises the rest.

Do not try to fix this by raising `OLLAMA_NUM_PARALLEL`. Every parallel slot
needs its own KV cache on a 12 GB card already holding 7.6 GB of weights, and
section 5.7 measured what happens when a model stops fitting: it lands on the
CPU and runs ten to thirteen times slower. The plausible-sounding optimisation
here makes the six-day job a sixty-day job.

The days are spent inside the GPU, and there is no core count that changes it.
What *can* be overlapped is the small CPU work either side of each call -
loading the thumbnail and base64-encoding it while the model is busy with the
previous one. At 2 s per call against a few milliseconds of encoding that is
worth about nothing today, and would only matter if the model ever got fast.

#### The rest

- **`validate`'s deep pass** re-decodes and re-hashes every texture inside a
  changed pack. Same pipeline, same fix, same ten-fold - and it is the same
  function, so do it once and both benefit.
- **`check_dungeondraft.py`** runs 13 independent scenes serially. It takes
  seconds; parallelising it would save seconds and make a failure harder to
  read. Leave it.
- **The assembler** is not a candidate. One map is milliseconds of work.
- **The app** is already right: every long job runs on a worker thread and the
  UI never blocks. There is one job at a time by design - `BeginJob` refuses a
  second - and that should stay, because these jobs contend for the same GPU,
  the same database and the same log.
- **The C++ side has nothing worth threading.** The rasterizer works on a
  50 x 40 grid, and the one heavy pixel loop - painting the bleed margin on a
  finished render - runs once beside a ComfyUI job measured in minutes.

The rule this leaves behind, which is the general one: **parallelise where the
work is CPU and the items are independent, and measure before and after.** Two
of the four candidates here looked equally promising and one of them was worth
exactly four per cent.

### Step 5 - the prop foundry

**Built** as `tools/dungeondraft_foundry.py`: Ideogram renders the prop through
ComfyUI, `rembg` cuts the background out, the result is scaled so its intended
grid footprint x 256 gives its pixel size, and it is written into our own pack
with a `pack.json`, a `default.dungeondraft_tags` and a `preview.png` through
our own Godot PCK writer. A procedural fallback draws the prop directly when
ComfyUI is not up.

What it is for, measured: `check_dungeondraft.py` reported 5 plan elements
across the 13 scenes that the library could not answer - all of them banners.
It reports 0 today, because the library grew, not because the foundry ran.

That number is small and it is honest. It was zero until the matcher stopped
inventing an answer: when the first two queries found nothing, a third returned
the first fifty objects in the table and picked one by hash, so a banner came
back as whatever happened to sort first. It looked like a match, it reported
as a match, and it would have been a wrong picture on somebody's map. Removing
it cost 5 objects out of 420 and made the check mean something.

One thing changed from the original step 5: the cutout is `rembg` with alpha
matting rather than BiRefNet `lucida`, because it is a pip install with no
model plumbing and the props are single objects on a plain ground, which is the
easy case for any matting model. Writing the pack format is no longer
theoretical: `dungeondraft_pck.py` has a `PckWriter`, and it has produced
`DBGProps01.dungeondraft_pack` - 2 props, registered in the index and enabled
in the Dungeondraft config. Whether Dungeondraft itself lists them is the next
thing to open the program for.

Reuse from the frozen branch: the `build_ideogram` and `make_cutout` graph
builders from `tools/tile_backends.py`, `prop_prompt` from `tools/tilegen.py`,
and `tools/check_library.py` as the quality gate.

### Step 6 - checks

**Done.** `python tools/check_dungeondraft.py` runs all thirteen scenes in
`tools/scenes/` through the assembler and reports, per scene: grid, walls,
doors, objects, plan elements with no asset, packs referenced, and a pass or
fail from validating the written JSON against section 2. It ends with the list
of kinds nothing in the library could answer - the prop foundry's work queue.

Current output: 13 pass, 251 walls, 415 objects, 23 doors, 5 elements with no
asset. This is the regression suite; keep it that way, and re-run it after
anything that touches the assembler or the matcher.

`python tools/check_enrichment_quality.py` is the other one, and it is a gate
rather than a regression: it scores a model on 200 sampled assets before that
model is allowed to write a quarter of a million descriptions.

## 10. Gotchas, each of which has already bitten

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

### Bitten during the implementation, not before it

Every one of these produced a file that validated, a job that exited zero, or a
green tick. None of them raised an error. That is the shape of every bug this
feature has had so far, and it is worth expecting more of the same.

- **A geometric test that can never be true.** A door's centre is *exactly*
  half a cell from the wall line, and the test asked for less than half a cell.
  Zero doors on thirteen scenes, no error, and a check that did not count
  doors. If a number comes out zero every time, that is a result to explain,
  not a default to accept.
- **A check whose fallback made it always pass.** The matcher's last resort
  returned an arbitrary asset, so nothing was ever unmatched, so the scene
  check reported nothing missing - forever. **A quiet substitution defeats the
  check built to catch it.** Return nothing and say so.
- **Two names for one file.** The tool wrote `map.report.json`; the app looked
  for `map.dungeondraft_map.report.json` and, finding nothing, showed zeros.
  The keys disagreed too - `walls_placed` against `placed_walls`. A sidecar
  read by two programs is a contract; section 8.3 now writes it down.
- **A retry with no cooldown.** The tab asked for library statistics whenever a
  flag said they were not loaded, and a failed read never set the flag - one
  python process per frame. A failure has to count as an attempt.
- **A cancel button that only set a flag.** Nothing read it, and the reader
  blocked. For a job measured in days that is not a cosmetic problem. Read the
  pipe with `PeekNamedPipe` and terminate the child.
- **Asking the model the same question twice.** Enrichment is keyed on content
  hash, but the work query returned one row per *asset*, and the same picture
  lives in several packs. Group by the hash.
- **`Path.with_suffix` replaces, `+` appends.** Both look right in a diff.
- **Godot counts weekdays from Sunday, Python from Monday.**
- **The build shipped the library.** A `copy_directory` of `data/` put 250 000
  thumbnails and an index of *this* machine's packs into every release, and
  `data/` was not in `.gitignore`. The catalogue is built on the user's machine
  from the packs they own; only the folder ships.

---

## 11. What to reuse, and what to leave alone

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

The work is on **`feature/dungeondraftintegration`**, branched from `main` as
this section originally instructed. It was entirely uncommitted for a while,
which this section used to warn about; it is committed now - the tools, the
format document, the Dungeondraft tab, and the geometry rebuild that followed
opening the first map.

`feature/tiled-generation` stays pushed, untouched, as the record of what was
tried and why it was stopped.

---

## 12. House style

This repository has conventions. Match them.

- Comments explain **why**, not what. If a line encodes a decision, say what
  the alternative was and why it lost.
- Prefer a named constant with a comment over a magic number.
- Every tool is runnable alone from the command line and prints something a
  human can read.
- Checks report numbers, not pictures — a wrong asset is a quiet failure.
- **Never add `Co-Authored-By` or any AI attribution to a commit message.**
  Commit messages end with the prose.
