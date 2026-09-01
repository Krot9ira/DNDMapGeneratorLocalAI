#!/usr/bin/env python3
"""Vision model asset enrichment pipeline.

Uses local Ollama vision models (e.g. gemma4:12b, context 4096) to generate
structured semantic descriptions, tags, object kinds, and footprints for
Dungeondraft assets from standardized thumbnails.
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from paths import ROOT as PROJECT_ROOT
from dungeondraft_db import AssetDatabase, DB_PATH_DEFAULT, THUMBS_DIR_DEFAULT
from ollama_client import OllamaClient, OllamaError

PROMPT_VERSION = 2
DEFAULT_CATALOGUER_MODEL = "gemma4:12b"
DEFAULT_PLANNER_MODEL = "qwen3.8:27b"

STYLE_TAGS_VOCABULARY = [
    "medieval", "dwarven", "elven", "gothic", "arcane", "ruined",
    "rustic", "coastal", "cyberpunk", "scifi", "natural", "ancient",
    "desert", "frozen", "volcanic", "underground", "oriental", "modern", "other",
]

SETTING_TAGS_VOCABULARY = [
    "tavern", "dungeon", "cave", "crypt", "temple", "castle",
    "library", "forest", "swamp", "mountain", "harbour", "ship",
    "city", "house", "mine", "sewer", "graveyard", "camp",
    "arena", "market", "wilds", "other",
]

# Bounded schema as specified in section 5.3 & 8.1 of PLAN_dungeondraft.md
ENRICHMENT_SCHEMA = {
    "type": "object",
    "properties": {
        "object_kind": {"type": "string", "maxLength": 40},
        "description": {"type": "string", "maxLength": 140},
        "semantic_tags": {
            "type": "array",
            "maxItems": 5,
            "items": {"type": "string", "maxLength": 24},
        },
        "style_tags": {
            "type": "array",
            "maxItems": 3,
            "items": {
                "type": "string",
                "enum": STYLE_TAGS_VOCABULARY,
            },
        },
        "setting_tags": {
            "type": "array",
            "maxItems": 3,
            "items": {
                "type": "string",
                "enum": SETTING_TAGS_VOCABULARY,
            },
        },
        "footprint": {
            "type": "string",
            "enum": ["floor", "wall-mounted", "ceiling", "overhang"],
        },
        "confidence": {"type": "number"},
    },
    "required": [
        "object_kind",
        "description",
        "semantic_tags",
        "style_tags",
        "setting_tags",
        "footprint",
        "confidence",
    ],
}

SYSTEM_PROMPT = """You catalogue art assets for a tabletop battlemap editor. You are shown one
asset on flat grey, drawn as seen from directly overhead. The file name and
folder are given as a hint from the artist; use them, but say what you can see
that they do not say.
object_kind: two or three plain words, lower case, no digits, no underscores.
Never repeat the file name.
description: one sentence under twenty words, naming colour and material.
style_tags: pick up to 3 from: medieval, dwarven, elven, gothic, arcane, ruined, rustic, coastal, cyberpunk, scifi, natural, ancient, desert, frozen, volcanic, underground, oriental, modern, other.
setting_tags: pick up to 3 from: tavern, dungeon, cave, crypt, temple, castle, library, forest, swamp, mountain, harbour, ship, city, house, mine, sewer, graveyard, camp, arena, market, wilds, other.
If the picture is too small or ambiguous to tell, use object_kind 'unclear'
and a low confidence rather than inventing."""


def check_vision_capability(base_url: str, model_name: str) -> Tuple[bool, str]:
    """Verify that the model exists in Ollama and has vision capability."""
    try:
        req = urllib.request.Request(
            f"{base_url.rstrip('/')}/api/show",
            data=json.dumps({"name": model_name}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            caps = data.get("capabilities", [])
            # Also check details or family
            if "vision" in caps:
                return True, "Model has vision capability."
            # Some versions might report in different fields
            return False, f"Model '{model_name}' does not have 'vision' capability (has {caps}). A vision model is required for cataloguing."
    except Exception as exc:
        return False, f"Could not verify model '{model_name}' with Ollama: {exc}"


def clean_object_kind(raw_kind: str) -> str:
    """Normalize object kind: lowercase, remove digits and underscores."""
    if not raw_kind:
        return "unclear"
    cleaned = re.sub(r"[\d_]+", " ", raw_kind).strip().lower()
    cleaned = re.sub(r"\s+", " ", cleaned)
    return cleaned or "unclear"


def is_echoing_filename(kind: str, file_name: str) -> bool:
    """Check if the model simply regurgitated the file stem."""
    stem = Path(file_name).stem.lower()
    clean_stem = re.sub(r"[\d_]+", " ", stem).strip()
    clean_stem = re.sub(r"\s+", " ", clean_stem)
    return kind.strip() == clean_stem.strip()


class DungeondraftEnricher:
    """Manages Ollama vision model enrichment for Dungeondraft assets."""

    def __init__(
        self,
        model: str = DEFAULT_CATALOGUER_MODEL,
        base_url: str = "http://127.0.0.1:11434",
        db_path: Optional[Path] = None,
        thumbs_dir: Optional[Path] = None,
    ):
        self.model = model
        self.base_url = base_url
        self.db = AssetDatabase(db_path=db_path, thumbs_dir=thumbs_dir)
        self.client = OllamaClient(base_url=base_url, model=model, timeout=120)

    def close(self):
        self.db.close()

    def verify_model(self) -> bool:
        """Check Ollama connectivity and vision support."""
        ok, msg = check_vision_capability(self.base_url, self.model)
        if not ok:
            print(f"Vision Verification Error: {msg}")
            return False
        return True

    def enrich_single(
        self,
        thumb_path: Path,
        file_name: str,
        category: str,
        subpath: str,
        width: int,
        height: int,
        grid_w: float,
        grid_h: float,
        pack_name: str = "",
        pack_tags: Optional[List[str]] = None,
    ) -> Optional[dict]:
        """Perform vision enrichment on a single asset thumbnail."""
        if not thumb_path.exists():
            return None

        prompt_context = [
            f"Folder: {subpath or category}",
            f"File name: {Path(file_name).stem}",
            f"Size: {width}x{height} px ({grid_w:.2f} x {grid_h:.2f} grid squares)",
        ]
        if pack_name:
            prompt_context.append(f"Pack: {pack_name}")
        if pack_tags:
            prompt_context.append(f"Tags: {', '.join(pack_tags)}")
        prompt_context.append("\nCatalogue this asset.")

        prompt_str = "\n".join(prompt_context)

        try:
            resp_text = self.client.generate(
                prompt=prompt_str,
                system=SYSTEM_PROMPT,
                format=ENRICHMENT_SCHEMA,
                images=[str(thumb_path)],
                temperature=0.0,
                think=False,
                num_predict=1500,
                num_ctx=4096,
            )

            data = json.loads(resp_text)
            raw_kind = data.get("object_kind", "unclear")
            kind = clean_object_kind(raw_kind)

            # Anti-echoing check: if model echoed filename, retry once with warning
            if is_echoing_filename(kind, file_name):
                retry_prompt = prompt_str + "\nNote: Do NOT repeat the filename. Name the actual physical object."
                resp_text = self.client.generate(
                    prompt=retry_prompt,
                    system=SYSTEM_PROMPT,
                    format=ENRICHMENT_SCHEMA,
                    images=[str(thumb_path)],
                    temperature=0.0,
                    think=False,
                    num_predict=1500,
                    num_ctx=4096,
                )
                data = json.loads(resp_text)
                kind = clean_object_kind(data.get("object_kind", "unclear"))
                if is_echoing_filename(kind, file_name):
                    # Still echoing: lower confidence
                    data["confidence"] = min(float(data.get("confidence", 0.5)), 0.3)

            data["object_kind"] = kind
            return data
        except Exception as exc:
            print(f"Error enriching {file_name}: {exc}")
            return None

    def run_pass(
        self,
        limit: Optional[int] = None,
        pack_id: Optional[str] = None,
        scope: str = "all",
        categories: Optional[List[str]] = None,
        non_objects_first: bool = True,
        sample_only: Optional[int] = None,
        recatalogue_suspect: bool = False,
    ) -> Dict[str, Any]:
        """Run enrichment pass over assets lacking enrichment or needing re-cataloguing."""
        if not self.verify_model():
            raise RuntimeError(f"Configured cataloguer model '{self.model}' is not suitable or reachable.")

        cur = self.db.conn.cursor()
        if recatalogue_suspect:
            where_condition = """
            a.state = 'ok' AND (
                e.content_hash IS NULL
                OR e.prompt_version < ?
                OR e.object_kind = 'unclear'
                OR e.confidence < 0.5
                OR e.semantic_tags = '[]'
                OR e.semantic_tags = ''
                OR e.description = ''
            )
            """
        else:
            where_condition = "a.state = 'ok' AND (e.content_hash IS NULL OR e.prompt_version < ?)"

        query = f"""
        SELECT a.content_hash, a.thumb_path, a.file_name, a.category, a.subpath,
               a.width, a.height, a.grid_w, a.grid_h, a.pack_tags, p.name as pack_name
        FROM assets a
        JOIN packs p ON a.pack_id = p.id
        LEFT JOIN enrichment e ON a.content_hash = e.content_hash
        WHERE {where_condition}
        """
        params: List[Any] = [PROMPT_VERSION]

        if pack_id:
            query += " AND a.pack_id = ?"
            params.append(pack_id)
        elif scope == "stock":
            query += " AND p.is_builtin = 1"
        elif scope == "custom":
            query += " AND p.is_builtin = 0"

        if categories:
            placeholders = ",".join("?" for _ in categories)
            query += f" AND a.category IN ({placeholders})"
            params.extend(categories)

        # One row per texture, not per placement of it. The same picture appears
        # in several packs, enrichment is keyed on its content hash, and asking
        # the model twice for the same image buys nothing but time.
        query += " GROUP BY a.content_hash"

        if non_objects_first:
            # Order non-objects first (terrain, walls, portals, etc. before objects)
            query += " ORDER BY (CASE WHEN a.category = 'objects' THEN 1 ELSE 0 END), a.category, a.id"
        else:
            query += " ORDER BY a.id"

        if sample_only:
            query += f" LIMIT {int(sample_only)}"
        elif limit:
            query += f" LIMIT {int(limit)}"

        cur.execute(query, params)
        rows = cur.fetchall()

        total = len(rows)
        print(f"\nStarting enrichment pass with model '{self.model}': {total} assets to process...")
        if total == 0:
            return {"processed": 0, "success": 0, "failed": 0}

        success = 0
        failed = 0
        t0 = time.time()

        for i, row in enumerate(rows, 1):
            c_hash = row["content_hash"]
            thumb_rel = row["thumb_path"]
            thumb_abs = self.db.thumbs_dir / thumb_rel

            p_tags = []
            try:
                p_tags = json.loads(row["pack_tags"] or "[]")
            except Exception:
                pass

            t_item = time.time()
            data = self.enrich_single(
                thumb_path=thumb_abs,
                file_name=row["file_name"],
                category=row["category"],
                subpath=row["subpath"] or "",
                width=row["width"],
                height=row["height"],
                grid_w=row["grid_w"],
                grid_h=row["grid_h"],
                pack_name=row["pack_name"] or "",
                pack_tags=p_tags,
            )

            if data:
                enr_record = {
                    "content_hash": c_hash,
                    "description": data.get("description", ""),
                    "object_kind": data.get("object_kind", "unclear"),
                    "semantic_tags": json.dumps(data.get("semantic_tags", [])),
                    "style_tags": json.dumps(data.get("style_tags", [])),
                    "setting_tags": json.dumps(data.get("setting_tags", [])),
                    "dominant_hue": "",
                    "footprint": data.get("footprint", "floor"),
                    "confidence": float(data.get("confidence", 0.5)),
                    "model": self.model,
                    "prompt_version": PROMPT_VERSION,
                    "created_at": int(time.time()),
                }
                self.db.upsert_enrichment(enr_record)
                success += 1
            else:
                failed += 1

            elapsed = time.time() - t0
            rate = i / elapsed if elapsed > 0 else 0
            remaining = (total - i) / rate if rate > 0 else 0

            if i % 10 == 0 or i == total:
                print(
                    f"  [{i}/{total}] {success} ok, {failed} failed "
                    f"({rate:.2f} items/s, ETA: {int(remaining // 60)}m {int(remaining % 60)}s) "
                    f"-> last: {data.get('object_kind', '') if data else 'fail'}",
                    flush=True,
                )

        print(f"\nEnrichment complete: {success} enriched, {failed} failed in {time.time() - t0:.1f}s.")
        return {"processed": total, "success": success, "failed": failed}


def main():
    parser = argparse.ArgumentParser(description="Dungeondraft Asset Vision Enrichment")
    parser.add_argument("--model", type=str, default=DEFAULT_CATALOGUER_MODEL, help="Ollama vision model name")
    parser.add_argument("--pack", type=str, help="Enrich only a specific pack ID (e.g. 'default')")
    parser.add_argument("--stock-only", action="store_true", help="Enrich only stock/builtin Dungeondraft assets")
    parser.add_argument("--custom-only", action="store_true", help="Enrich only custom asset packs")
    parser.add_argument("--scope", choices=["all", "stock", "custom"], default="all", help="Scope of assets to enrich")
    parser.add_argument("--limit", type=int, help="Limit number of assets to process")
    parser.add_argument("--sample", type=int, help="Run on a sample of N assets")
    parser.add_argument("--category", type=str, nargs="+", help="Specific categories to enrich")
    parser.add_argument("--recatalogue-suspect", action="store_true", help="Re-catalogue suspect or low confidence entries")

    args = parser.parse_args()

    scope = args.scope
    if args.stock_only:
        scope = "stock"
    elif args.custom_only:
        scope = "custom"

    enricher = DungeondraftEnricher(model=args.model)
    try:
        enricher.run_pass(
            limit=args.limit,
            pack_id=args.pack,
            scope=scope,
            categories=args.category,
            sample_only=args.sample,
            recatalogue_suspect=getattr(args, "recatalogue_suspect", False),
        )
    finally:
        enricher.close()


if __name__ == "__main__":
    main()
