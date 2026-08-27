# HANDOFF: Ideogram 4 Schema Compliance and Quality Resolution

## 1. Executive Summary

- **Status:** **RESOLVED / WORKING AS DESIGNED**
- **Core Diagnosis:** Previous attempts treated prompt generation as generic prompt engineering rather than adhering to the **formal Ideogram 4 training contract**. Four major structural defects existed in the JSON caption pipeline:
  1. **Missing Required Schema Key:** `style_description` was missing the required `art_style` key (mandatory for non-photographic outputs). Without it, Ideogram 4 operates out-of-distribution and relies on unconstrained prior bias.
  2. **Severe Distribution Mismatch in `high_level_description`:** `high_level_description` had grown to 350+ words packed with repetitive negative wall constraints and multi-paragraph essays. Per Ideogram 4 documentation, `high_level_description` must be **1–3 concise sentences**. Dense structural and global constraints belong in `compositional_deconstruction.background`.
  3. **Atmospheric Effect Bounding Box Leakage:** Spanning effects (e.g. street embers spanning 44 columns) were emitted as giant bounding boxes (`[571, 77, 810, 923]`), prompting the DiT to interpret them as building footprints.
  4. **Outdoor Prop Bounding Box Perimeter:** Unlabelled props in open outdoor plazas formed accidental bounding box rings around open areas, leading to balustrade hallucinations.

---

## 2. Changes Implemented

### A. Ideogram 4 JSON Schema Adherence (`tools/ideogram_prompt.py`, `app/include/ideogram_caption.h`)
- Added required `art_style` field to `style_description` in both Python and C++ with strict key ordering: `aesthetics` -> `lighting` -> `art_style` -> `medium` -> `color_palette`.
- Added default `art_style` to `styles/_base.json` and `StyleDef` / `BaseStyle` structures.

### B. High-Level Description Pruning & Background Relocation
- `high_level_description` is now strictly 2–3 sentences: scene summary, positive negative-constraint, and key layout landmarks.
- All structural constraints (undivided space, wall faces, viewpoint note, building counts, anti-mirroring, orthographic view) were moved to `compositional_deconstruction.background`.

### C. Bounding Box Cleanup
- Reduced effect atmosphere threshold to `>= 0.18 * cols * rows` or `>= 0.55 * cols` / `rows`, preventing map-spanning effects from becoming building boxes.
- Suppressed 1x1 bounding boxes for unlabelled generic props on open outdoor ground, routing them to `loose` clutter.

### D. Automated Test Suite & Dual Port Parity
- 100% byte-for-byte caption parity between C++ (`DndBattlemapGenerator.exe`) and Python across all 14 layouts (`tools/check_caption_parity.py`).
- All 13 test scenes pass validation (`tools/check_scenes.py`).
- Generator sanity across all layouts, sizes, and seeds passes (`tools/check_layouts.py`).
- Style linting clean (`tools/check_captions.py`).

---

## 3. Render Validation Results (5 ComfyUI Generations)

| Render # | Scene | Key Changes Tested | Output File | Result & Analysis |
|---|---|---|---|---|
| **1/5** | `burning_quarter` | `art_style` schema added | `battlemap_00123_.png` | Correct art style, but embers & prop bounding boxes still created lower phantom structures. |
| **2/5** | `burning_quarter` | Atmosphere threshold + outdoor prop suppression | `battlemap_00124_.png` | **Huge improvement**: South street opened up completely with toppled cart/fire; Stone House became single undivided space. |
| **3/5** | `burning_quarter` | Concise 3-sentence `high_level_description` + background cleanup | `battlemap_00125_.png` | **Excellent visual clarity**: Street fully open, crisp textures, proper orthographic view. |
| **4/5** | `goblin_gorge` | Open rock canyon, cliffs, central fire & barricade | `battlemap_00126_.png` | **Flawless outdoor render**: Sheer cliffs on east/west, central campfire with pot, barricade across entrance, zero unwanted buildings. |
| **5/5** | `tavern` | Single undivided indoor common room with bar & stair | `battlemap_00127_.png` | **Flawless indoor render**: 100% undivided hall from wall to wall, zero partition walls, split table in center, long bar on left, stair on right. |

---

## 4. Operational Guidelines for Future Development

1. **Always keep `high_level_description` concise (1–3 sentences).** Never append multi-paragraph essays to `high_level_description`.
2. **Never emit bounding boxes for open ground or generic clutter.** Use `compositional_deconstruction.background` for ground textures and loose clutter descriptions for unpinned props.
3. **Keep C++ and Python in sync.** Run `python tools/check_caption_parity.py` before and after any caption changes.

