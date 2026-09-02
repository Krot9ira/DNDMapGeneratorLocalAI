#!/usr/bin/env python3
"""Dungeondraft Prop Foundry.

Generates custom standalone props on transparent backgrounds for map plan elements
that cannot be satisfied by the user's asset library, cuts out the background,
scales to 256px/grid cell, and packages them into a native .dungeondraft_pack.
"""
import argparse
import hashlib
import io
import json
import math
import os
import random
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from PIL import Image, ImageDraw, ImageFilter

from paths import ROOT as PROJECT_ROOT
from dungeondraft_db import (
    AssetDatabase,
    analyze_image,
    compute_asset_id,
    save_thumbnail,
    DB_PATH_DEFAULT,
    THUMBS_DIR_DEFAULT,
)
from dungeondraft_pck import PckWriter
from dungeondraft_indexer import read_dungeondraft_config, enable_pack_in_dungeondraft_config

PACK_ID = "DBGProps01"
PACK_NAME = "DndBattlemapGenerator Custom Props"
PACK_AUTHOR = "DndBattlemapGenerator"
GENERATED_DIR_DEFAULT = PROJECT_ROOT / "data" / "generated_props"


def make_procedural_prop(
    kind: str,
    style: str = "default",
    grid_w: float = 1.0,
    grid_h: float = 1.0,
    seed: int = 42,
) -> Image.Image:
    """Generate a clean top-down prop sprite with transparent background.

    Produces a high-resolution 256px/grid RGBA sprite matching tabletop aesthetics.
    """
    random.seed(seed)
    pw = int(max(64, round(grid_w * 256)))
    ph = int(max(64, round(grid_h * 256)))

    img = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    margin_x = int(pw * 0.1)
    margin_y = int(ph * 0.1)
    bx0, by0 = margin_x, margin_y
    bx1, by1 = pw - margin_x, ph - margin_y

    kind_lower = kind.lower()

    if "banner" in kind_lower or "flag" in kind_lower:
        # Top-down banner / tapestry hanging or laid flat
        # Rich fabric with gold embroidery and pole
        pole_color = (90, 60, 30, 255)
        fabric_color = (140, 25, 35, 255) if "gothic" in style or "crypt" in style else (30, 60, 140, 255)
        trim_color = (210, 175, 55, 255)

        # Pole
        draw.rounded_rectangle([bx0, by0, bx1, by0 + int(ph * 0.15)], radius=4, fill=pole_color, outline=(40, 25, 10, 255), width=2)
        # Finials
        draw.ellipse([bx0 - 4, by0 - 2, bx0 + 6, by0 + int(ph * 0.15) + 2], fill=trim_color)
        draw.ellipse([bx1 - 6, by0 - 2, bx1 + 4, by0 + int(ph * 0.15) + 2], fill=trim_color)
        # Hanging cloth
        cloth_pts = [
            (bx0 + 6, by0 + int(ph * 0.12)),
            (bx1 - 6, by0 + int(ph * 0.12)),
            (bx1 - 6, by1 - int(ph * 0.15)),
            ((bx0 + bx1) // 2, by1),
            (bx0 + 6, by1 - int(ph * 0.15)),
        ]
        draw.polygon(cloth_pts, fill=fabric_color, outline=(20, 20, 20, 255))
        # Gold heraldry / trim
        inner_pts = [
            (bx0 + 14, by0 + int(ph * 0.2)),
            (bx1 - 14, by0 + int(ph * 0.2)),
            (bx1 - 14, by1 - int(ph * 0.22)),
            ((bx0 + bx1) // 2, by1 - int(ph * 0.1)),
            (bx0 + 14, by1 - int(ph * 0.22)),
        ]
        draw.polygon(inner_pts, fill=None, outline=trim_color, width=2)
        # Emblem in centre
        cx, cy = (bx0 + bx1) // 2, (by0 + by1) // 2
        draw.ellipse([cx - 12, cy - 12, cx + 12, cy + 12], fill=trim_color, outline=(60, 40, 10, 255), width=1)

    elif "altar" in kind_lower or "shrine" in kind_lower:
        # Carved stone altar
        stone_base = (70, 70, 75, 255)
        stone_top = (110, 110, 115, 255)
        draw.rounded_rectangle([bx0, by0, bx1, by1], radius=8, fill=stone_base, outline=(30, 30, 35, 255), width=3)
        draw.rounded_rectangle([bx0 + 8, by0 + 8, bx1 - 8, by1 - 8], radius=6, fill=stone_top, outline=(45, 45, 50, 255), width=2)
        # Carved runes / cloth runner
        cx, cy = (bx0 + bx1) // 2, (by0 + by1) // 2
        draw.rectangle([cx - 15, by0 + 4, cx + 15, by1 - 4], fill=(130, 20, 30, 230), outline=(200, 160, 40, 255), width=1)

    elif "rune" in kind_lower or "circle" in kind_lower:
        # Magic circle / glowing runes
        cx, cy = pw // 2, ph // 2
        r = min(pw, ph) // 2 - 10
        glow = (120, 70, 220, 220) if "arcane" in style else (60, 180, 220, 220)
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=glow, width=3)
        draw.ellipse([cx - r + 12, cy - r + 12, cx + r - 12, cy + r - 12], outline=glow, width=2)
        # Inscribed star
        for a in range(0, 360, 72):
            rad = math.radians(a)
            rad2 = math.radians((a + 144) % 360)
            x1, y1 = cx + (r - 16) * math.cos(rad), cy + (r - 16) * math.sin(rad)
            x2, y2 = cx + (r - 16) * math.cos(rad2), cy + (r - 16) * math.sin(rad2)
            draw.line([(x1, y1), (x2, y2)], fill=glow, width=2)

    else:
        # Generic detailed tabletop wooden / stone feature
        draw.rounded_rectangle([bx0, by0, bx1, by1], radius=6, fill=(110, 80, 50, 255), outline=(40, 25, 10, 255), width=3)
        draw.rounded_rectangle([bx0 + 6, by0 + 6, bx1 - 6, by1 - 6], radius=4, fill=(140, 105, 70, 255), outline=(60, 40, 20, 255), width=2)
        draw.line([bx0 + 6, (by0 + by1) // 2, bx1 - 6, (by0 + by1) // 2], fill=(60, 40, 20, 255), width=2)

    # Soft drop shadow underneath
    shadow = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
    sdraw = ImageDraw.Draw(shadow)
    sdraw.rounded_rectangle([bx0 + 4, by0 + 6, bx1 + 4, by1 + 6], radius=6, fill=(0, 0, 0, 100))
    shadow = shadow.filter(ImageFilter.GaussianBlur(radius=4))

    final_img = Image.alpha_composite(shadow, img)
    return final_img


def make_prop_ideogram_caption(kind: str, style: str = "default", description: str = "") -> str:
    """Build Ideogram 4 spatial JSON caption for rendering an isolated tabletop prop."""
    desc = description or f"A detailed {kind.replace('_', ' ')} for a tabletop battlemap"
    caption_dict = {
        "background": "A solid plain flat pure uniform white background with zero texture, zero shadow, zero gradient.",
        "elements": [
            {
                "box_2d": [100, 100, 900, 900],
                "label": kind.replace("_", " "),
                "description": (
                    f"Top-down directly overhead view of a single isolated {kind.replace('_', ' ')}, "
                    f"{desc}, {style} style, tabletop RPG battlemap prop, completely centered, "
                    f"crisp clean edges, isolated on solid white background, no floor, no people, no creatures."
                ),
            }
        ],
    }
    return json.dumps(caption_dict, ensure_ascii=False)


def render_comfy_prop(
    kind: str,
    style: str = "default",
    description: str = "",
    grid_w: float = 1.0,
    grid_h: float = 1.0,
    seed: int = 42,
    base_url: str = "http://127.0.0.1:8188",
) -> Optional[Image.Image]:
    """Attempt to render a standalone prop using ComfyUI Ideogram 4 + Rembg cutout."""
    try:
        from comfy import ComfyClient
        from workflow import build_ideogram4

        client = ComfyClient(base_url=base_url)
        ok, _ = client.health()
        if not ok:
            return None

        # Load app config
        config_path = PROJECT_ROOT / "config.json"
        cfg = json.loads(config_path.read_text(encoding="utf-8")) if config_path.exists() else {}

        # Use fast turbo/default preset for single prop sprite
        ideogram_cfg = dict(cfg.get("ideogram", {}))
        ideogram_cfg["preset"] = "Turbo"
        ideogram_cfg["steps"] = 16
        cfg_prop = dict(cfg)
        cfg_prop["ideogram"] = ideogram_cfg

        caption_json = make_prop_ideogram_caption(kind=kind, style=style, description=description)
        graph = build_ideogram4(cfg_prop, caption_json=caption_json, seed=seed, width=768, height=768)

        print(f"[foundry] Queuing Ideogram render in ComfyUI for prop '{kind}'...")
        prompt_id = client.queue_prompt(graph)
        outputs = client.wait(prompt_id, timeout=300)

        # Download rendered image
        import tempfile
        with tempfile.TemporaryDirectory() as tmpdir:
            images = client.get_images(outputs, tmpdir)
            if not images:
                return None
            raw_img = Image.open(images[0]).convert("RGBA")

        # Background cutout via rembg
        try:
            import rembg
            cutout_img = rembg.remove(raw_img, alpha_matting=True)
        except Exception:
            cutout_img = raw_img

        # Crop to content bounding box
        bbox = cutout_img.getbbox()
        if bbox:
            cutout_img = cutout_img.crop(bbox)

        # Scale to fit intended grid cell dimensions (256px / cell)
        target_w = int(max(64, round(grid_w * 256)))
        target_h = int(max(64, round(grid_h * 256)))

        cur_w, cur_h = cutout_img.size
        scale = min(target_w / cur_w, target_h / cur_h)
        scaled_w = max(1, int(cur_w * scale))
        scaled_h = max(1, int(cur_h * scale))
        resized = cutout_img.resize((scaled_w, scaled_h), Image.LANCZOS)

        # Place onto transparent canvas
        final_canvas = Image.new("RGBA", (target_w, target_h), (0, 0, 0, 0))
        off_x = (target_w - scaled_w) // 2
        off_y = (target_h - scaled_h) // 2
        final_canvas.paste(resized, (off_x, off_y), resized)

        return final_canvas
    except Exception as exc:
        print(f"[foundry] ComfyUI generation failed for {kind} ({exc}); using procedural generator")
        return None


class PropFoundry:
    """Foundry for creating, cataloguing and packaging custom props into Dungeondraft packs."""

    def __init__(
        self,
        output_dir: Optional[Path] = None,
        db_path: Optional[Path] = None,
    ):
        self.output_dir = output_dir or GENERATED_DIR_DEFAULT
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.db = AssetDatabase(db_path=db_path)
        self._ensure_pack_registered()

    def _ensure_pack_registered(self):
        pack_dict = {
            "id": PACK_ID,
            "name": PACK_NAME,
            "author": PACK_AUTHOR,
            "version": "1.0",
            "file_path": str(self.output_dir.parent / f"{PACK_ID}.dungeondraft_pack"),
            "file_size": 0,
            "file_mtime": int(time.time()),
            "is_builtin": 0,
            "enabled": 1,
            "duplicate_of": None,
            "indexed_at": int(time.time()),
            "scan_version": 1,
        }
        self.db.upsert_pack(pack_dict)

    def close(self):
        self.db.close()

    def generate_prop(
        self,
        kind: str,
        style: str = "default",
        description: str = "",
        grid_w: float = 1.0,
        grid_h: float = 1.0,
        seed: int = 42,
        force_procedural: bool = False,
    ) -> Dict[str, Any]:
        """Generate a single prop, write PNG, and record in generated_props table."""
        clean_name = kind.lower().replace(" ", "_")
        clean_style = style.lower().replace(" ", "_")
        prop_id_str = f"{clean_name}_{clean_style}_{grid_w}x{grid_h}_{seed}"
        prop_id = hashlib.sha256(prop_id_str.encode("utf-8")).hexdigest()[:16]

        prop_filename = f"{clean_name}_{prop_id[:8]}.png"
        style_dir = self.output_dir / clean_style
        style_dir.mkdir(parents=True, exist_ok=True)
        png_path = style_dir / prop_filename

        # 1. Try ComfyUI Ideogram + Rembg cutout
        img = None
        backend = "procedural_foundry"
        if not force_procedural:
            img = render_comfy_prop(
                kind=kind,
                style=style,
                description=description,
                grid_w=grid_w,
                grid_h=grid_h,
                seed=seed,
            )
            if img is not None:
                backend = "ideogram_comfyui_rembg"

        # 2. Fallback to procedural generator
        if img is None:
            img = make_procedural_prop(kind=kind, style=style, grid_w=grid_w, grid_h=grid_h, seed=seed)

        img.save(png_path, "PNG")

        metrics = analyze_image(img)
        thumb_rel = save_thumbnail(img, metrics["content_hash"], self.db.thumbs_dir)

        desc = description or f"A top-down detailed {kind.replace('_', ' ')} suited for {style} battlemaps."

        record = {
            "id": prop_id,
            "name": kind,
            "description": desc,
            "style_id": style,
            "png_path": str(png_path),
            "grid_w": grid_w,
            "grid_h": grid_h,
            "backend": backend,
            "seed": seed,
            "packed_into": PACK_ID,
            "created_at": int(time.time()),
        }
        self.db.upsert_generated_prop(record)

        # Also register directly in assets & enrichment tables so matcher finds it
        res_path = f"res://packs/{PACK_ID}/textures/objects/{clean_style}/{prop_filename}"
        asset_id = compute_asset_id(PACK_ID, res_path)
        asset_dict = {
            "id": asset_id,
            "pack_id": PACK_ID,
            "res_path": res_path,
            "category": "objects",
            "subpath": f"generated/{clean_style}",
            "file_name": prop_filename,
            "width": metrics["width"],
            "height": metrics["height"],
            "grid_w": grid_w,
            "grid_h": grid_h,
            "has_alpha": 1,
            "alpha_coverage": metrics["alpha_coverage"],
            "mean_rgb": metrics["mean_rgb"],
            "palette": metrics["palette"],
            "thumb_path": thumb_rel,
            "pack_tags": json.dumps(["Generated", kind]),
            "pack_sets": json.dumps(["DndBattlemapGenerator"]),
            "content_hash": metrics["content_hash"],
            "state": "ok",
            "last_seen_at": int(time.time()),
        }
        self.db.upsert_asset(asset_dict)

        enr_dict = {
            "content_hash": metrics["content_hash"],
            "description": desc,
            "object_kind": kind.lower().replace("_", " "),
            "semantic_tags": json.dumps(["generated", kind, clean_style]),
            "style_tags": json.dumps([clean_style]),
            "setting_tags": json.dumps(["any"]),
            "dominant_hue": "",
            "footprint": "floor",
            "confidence": 0.95,
            "model": "foundry",
            "prompt_version": 2,
            "created_at": int(time.time()),
        }
        self.db.upsert_enrichment(enr_dict)

        print(f"Generated prop '{kind}' ({grid_w}x{grid_h} sq) -> {png_path}")
        return record

    def build_custom_pack(self, target_pack_path: Optional[str] = None) -> str:
        """Package all generated props into a native .dungeondraft_pack."""
        config = read_dungeondraft_config()
        cad = config.get("custom_assets_directory")

        if target_pack_path:
            pack_out = Path(target_pack_path)
        elif cad and os.path.exists(cad):
            pack_out = Path(cad) / f"{PACK_ID}.dungeondraft_pack"
        else:
            pack_out = self.output_dir.parent / f"{PACK_ID}.dungeondraft_pack"

        writer = PckWriter(str(pack_out))

        # 1. pack.json
        pack_manifest = {
            "name": PACK_NAME,
            "id": PACK_ID,
            "version": "1.0",
            "author": PACK_AUTHOR,
            "allow_3rd_party_mapping_software_to_read": True,
            "custom_color_overrides": {"enabled": False},
        }
        writer.add_file(f"res://packs/{PACK_ID}/pack.json", json.dumps(pack_manifest, indent=2).encode("utf-8"))

        # 2. Preview image (256x320)
        preview_img = Image.new("RGBA", (256, 320), (35, 30, 45, 255))
        pdraw = ImageDraw.Draw(preview_img)
        pdraw.rectangle([10, 10, 246, 310], outline=(200, 170, 60, 255), width=2)
        pdraw.text((30, 140), "Custom Props", fill=(240, 240, 240, 255))
        pbuf = io.BytesIO()
        preview_img.save(pbuf, "PNG")
        writer.add_file(f"res://packs/{PACK_ID}/preview.png", pbuf.getvalue())

        # 3. Tags & Textures
        tag_map: Dict[str, List[str]] = {"Generated": []}
        all_pngs = list(self.output_dir.rglob("*.png"))

        for png in all_pngs:
            rel_style = png.parent.name
            tex_rel = f"textures/objects/{rel_style}/{png.name}"
            res_path = f"res://packs/{PACK_ID}/{tex_rel}"
            writer.add_from_disk(res_path, str(png))
            tag_map["Generated"].append(tex_rel)

        tags_data = {
            "tags": tag_map,
            "sets": {"DndBattlemapGenerator": ["Generated"]},
        }
        writer.add_file(f"res://packs/{PACK_ID}/data/default.dungeondraft_tags", json.dumps(tags_data, indent=2).encode("utf-8"))

        writer.write()

        # Update pack row in DB
        stat = os.stat(str(pack_out))
        pack_dict = {
            "id": PACK_ID,
            "name": PACK_NAME,
            "author": PACK_AUTHOR,
            "version": "1.0",
            "file_path": os.path.abspath(str(pack_out)),
            "file_size": stat.st_size,
            "file_mtime": int(stat.st_mtime),
            "is_builtin": 0,
            "enabled": 1,
            "duplicate_of": None,
            "indexed_at": int(time.time()),
            "scan_version": 1,
        }
        self.db.upsert_pack(pack_dict)

        # Enable pack in Dungeondraft's config.ini so Dungeondraft loads it without 'missing pack' caution
        enable_pack_in_dungeondraft_config(PACK_ID)

        print(f"Packed {len(all_pngs)} textures into Dungeondraft pack: {pack_out}")
        return str(pack_out)

    def satisfy_unmatched(self, unmatched_list: List[dict], style: str = "default") -> int:
        """Generate and pack all unmatched props from a report."""
        created = 0
        for item in unmatched_list:
            kind = item.get("kind", "prop")
            self.generate_prop(kind=kind, style=style, grid_w=1.0, grid_h=1.0)
            created += 1
        if created > 0:
            self.build_custom_pack()
        return created


def main():
    parser = argparse.ArgumentParser(description="Dungeondraft Prop Foundry")
    subparsers = parser.add_subparsers(dest="command", help="Command to run")

    gen_parser = subparsers.add_parser("generate", help="Generate a single prop")
    gen_parser.add_argument("--prop", type=str, required=True, help="Prop kind/name (e.g. banner, altar)")
    gen_parser.add_argument("--style", type=str, default="default", help="Map style ID")
    gen_parser.add_argument("--width", type=float, default=1.0, help="Width in grid cells")
    gen_parser.add_argument("--height", type=float, default=1.0, help="Height in grid cells")
    gen_parser.add_argument("--seed", type=int, default=42, help="Random seed")

    pack_parser = subparsers.add_parser("pack", help="Build .dungeondraft_pack from all generated props")
    pack_parser.add_argument("--out", type=str, help="Output .dungeondraft_pack path")

    batch_parser = subparsers.add_parser("satisfy", help="Satisfy unmatched props from a .report.json file")
    batch_parser.add_argument("report_file", type=str, help="Path to .report.json")

    args = parser.parse_args()
    foundry = PropFoundry()

    try:
        if args.command == "generate":
            foundry.generate_prop(
                kind=args.prop,
                style=args.style,
                grid_w=args.width,
                grid_h=args.height,
                seed=args.seed,
            )
            foundry.build_custom_pack()
        elif args.command == "pack":
            foundry.build_custom_pack(target_pack_path=getattr(args, "out", None))
        elif args.command == "satisfy":
            report_path = Path(args.report_file)
            if not report_path.exists():
                sys.exit(f"Report file not found: {report_path}")
            data = json.loads(report_path.read_text(encoding="utf-8"))
            unmatched = data.get("unmatched_props", [])
            style = data.get("style", "default")
            count = foundry.satisfy_unmatched(unmatched, style=style)
            print(f"Satisfied {count} unmatched props for {report_path.name}.")
        else:
            parser.print_help()
    finally:
        foundry.close()


if __name__ == "__main__":
    main()
