# Dungeondraft Map Format Reference (.dungeondraft_map)

This document specifies the exact structure of `.dungeondraft_map` files (Godot 3 JSON format). All values and conventions documented here have been verified against real Dungeondraft map files and official assets.

---

## 1. Units and Coordinates

- **Resolution:** 256 pixels = 1 grid square (cell).
- **Origin:** `(0, 0)` is the top-left corner of the playable area.
- **Angles:** All rotations are in **radians** ($0$ to $2\pi$). $\pi/2 \approx 1.570796$, $\pi/4 \approx 0.785398$.
- **Data Types:** Vector and array types are serialized as custom Godot strings:
  - Vector2: `"Vector2( 960.533, 1052.2 )"`
  - PoolVector2Array: `"PoolVector2Array( x1, y1, x2, y2, ... )"` (flat coordinate pairs)
  - PoolIntArray: `"PoolIntArray( 0, 1, 2, -1, ... )"`
  - PoolByteArray: `"PoolByteArray( 0, 255, 128, ... )"`
- **Node IDs:** Every entity (wall, portal, object, light, roof, path) has a `node_id` string formatted as a **lowercase hexadecimal number** (e.g. `"0"`, `"7"`, `"1a"`). `world.next_node_id` must be strictly greater than the maximum `node_id` used.

---

## 2. Top-Level Structure

```json
{
  "header": {
    "creation_build": "1.1.0.0 newborn phoenix",
    "creation_date": {
      "year": 2026, "month": 8, "day": 30, "weekday": 0, "dst": false,
      "hour": 12, "minute": 0, "second": 0
    },
    "uses_default_assets": true,
    "asset_manifest": [
      {
        "name": "Pack Name",
        "id": "PackID8c",
        "version": "1.0",
        "author": "Author",
        "keywords": null,
        "allow_3rd_party_mapping_software_to_read": true,
        "custom_color_overrides": {}
      }
    ],
    "editor_state": {
      "camera_offset": "Vector2( 0, 0 )",
      "camera_zoom": 1.0,
      "grid_opacity": 0.5
    }
  },
  "world": {
    "format": 3,
    "width": 25,
    "height": 30,
    "next_node_id": "1e",
    "next_prefab_id": "0",
    "msi": {
      "offset_map_size": 1,
      "max_offset_distance": 0.0,
      "cell_size": 256.0,
      "seed": 0
    },
    "grid": {
      "color": "ff000000",
      "texture": "res://textures/grid/square.png"
    },
    "wall_shadow": true,
    "object_shadow": true,
    "building_wear": true,
    "trace_image_visible": false,
    "embedded": {},
    "levels": {
      "0": { ... }
    }
  }
}
```

---

## 3. Level 0 Structure

### 3.1 Layers
Dungeondraft standard layer IDs:
```json
{
  "-400": "Below Ground",
  "-100": "Below Water",
  "100": "Layer 1 (Objects)",
  "200": "Layer 2 (Paths)",
  "300": "Layer 3 (Patterns)",
  "700": "Above Walls",
  "900": "Above Roofs"
}
```

### 3.2 Tiles (Floor Grid)
`tiles.cells` is a row-major 1D array of length `width * height`:
- `-1`: Empty cell (shows underlying terrain / background).
- `>= 0`: Index into `tiles.lookup`.

```json
"tiles": {
  "cells": "PoolIntArray( 0, 0, 0, -1, -1, ... )",
  "colors": {},
  "lookup": {
    "0": "res://textures/tilesets/tileset_wood_planks.png"
  }
}
```

### 3.3 Terrain (Splatmaps)
Supports up to 8 terrain textures. Splats sample at **4 samples per grid cell per axis**:
- Sampling grid: `(4 * width)` by `(4 * height)` points.
- `splat` byte array length: `4 * (4 * width) * (4 * height)` (RGBA channels = textures 1 to 4 weights, 0..255).
- `splat2` byte array length: `4 * (4 * width) * (4 * height)` (textures 5 to 8 weights).

```json
"terrain": {
  "enabled": true,
  "expand_slots": false,
  "smooth_blending": true,
  "texture_1": "res://textures/terrain/cobblestone.png",
  "texture_2": "res://textures/terrain/grass.png",
  "texture_3": "",
  "texture_4": "",
  "texture_5": "",
  "texture_6": "",
  "texture_7": "",
  "texture_8": "",
  "splat": "PoolByteArray( 255, 0, 0, 0, ... )",
  "splat2": "PoolByteArray( 0, 0, 0, 0, ... )"
}
```

### 3.4 Walls and Portals
Walls are polylines in absolute pixel coordinates.
- `type: 0`: Wall bounding a building floor shape (mirrors `shapes.polygons`).
- `type: 1`: Free-drawn wall.
- `joint: 1`, `normalize_uv: true`, `shadow: true`.
- **Portals** are attached inside the wall's `portals` array:
  - `point_index`: Segment index along the wall polyline.
  - `wall_distance`: Parametric fractional distance ($0.0 \dots 1.0$) along that segment.
  - `radius`: 128 (half cell = 1 cell door width).

```json
"walls": [
  {
    "points": "PoolVector2Array( 2304, 1536, 2304, 4352, 3328, 4352, 3328, 1536 )",
    "texture": "res://textures/walls/sample_wall.png",
    "color": "ffffffff",
    "loop": true,
    "type": 0,
    "joint": 1,
    "normalize_uv": true,
    "shadow": true,
    "node_id": "2",
    "portals": [
      {
        "position": "Vector2( 2304, 3712 )",
        "rotation": 1.570796,
        "scale": "Vector2( 1, 1 )",
        "direction": "Vector2( 0, 1 )",
        "texture": "res://textures/portals/sample_door.png",
        "radius": 128,
        "point_index": 0,
        "wall_id": "0",
        "wall_distance": 0.772727,
        "closed": true,
        "node_id": "7"
      }
    ]
  }
]
```

### 3.5 Shapes (Building Floors)
- `shapes.polygons`: List of `PoolVector2Array` floor outlines.
- `shapes.walls`: Parallel list of **decimal integer** representations of the bounding wall's `node_id` (which is written in hex on the wall entity itself).

### 3.6 Objects (Props)
Placed assets with physical scale (unpadded, 256 authored px = 1 grid square).
```json
"objects": [
  {
    "position": "Vector2( 960.533, 1052.2 )",
    "rotation": 1.832596,
    "scale": "Vector2( 1, 1 )",
    "mirror": false,
    "texture": "res://textures/objects/supplies/crates/fruit_box_05.png",
    "layer": 100,
    "shadow": false,
    "custom_color": "ff87a868",
    "node_id": "a"
  }
]
```

### 3.7 Lights
Light sources with range in grid cells and ARGB hex color.
```json
"lights": [
  {
    "position": "Vector2( 2727.99, 2860.73 )",
    "rotation": 0,
    "range": 5.0,
    "intensity": 1.0,
    "color": "ffeccd8b",
    "texture": "res://textures/lights/fragments.png",
    "shadows": true,
    "node_id": "19"
  }
]
```

### 3.8 Caves
Decoded as 1 bit per sample, packed 8 bits per byte, **least-significant bit first**, row-major.
- Grid size: `(4 * width + 3) * (4 * height + 3)` samples.
- Byte count: `ceil( (4 * width + 3) * (4 * height + 3) / 8 )`.
```json
"cave": {
  "bitmap": "PoolByteArray( ... )",
  "entrance_bitmap": "PoolByteArray( ... )",
  "ground_color": "ff3a352c",
  "wall_color": "ff1b1712",
  "texture": "res://textures/caves/colorable/floor.png"
}
```
