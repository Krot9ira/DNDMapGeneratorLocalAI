#!/usr/bin/env python3
"""SQLite database and image analysis for Dungeondraft assets.

Manages assets.db, computes content hashes, extracts image dimensions and
dominant palettes, generates standardized thumbnails, and manages enrichment
records.
"""
import hashlib
import json
import os
import sqlite3
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from PIL import Image

try:
    import numpy as np
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False

from paths import ROOT as PROJECT_ROOT

SCHEMA_VERSION = 1
DB_PATH_DEFAULT = PROJECT_ROOT / "data" / "assets.db"
THUMBS_DIR_DEFAULT = PROJECT_ROOT / "data" / "thumbnails"


def init_db(db_path: Optional[Path] = None) -> sqlite3.Connection:
    """Initialize SQLite database and create tables if they do not exist."""
    path = Path(db_path or DB_PATH_DEFAULT)
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(path), timeout=30.0)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL;")
    conn.execute("PRAGMA synchronous=NORMAL;")
    conn.execute("PRAGMA foreign_keys=ON;")

    with conn:
        conn.executescript("""
        CREATE TABLE IF NOT EXISTS schema_version (
            version INTEGER PRIMARY KEY
        );

        CREATE TABLE IF NOT EXISTS packs (
            id            TEXT PRIMARY KEY,
            name          TEXT NOT NULL,
            author        TEXT,
            version       TEXT,
            file_path     TEXT NOT NULL,
            file_size     INTEGER NOT NULL,
            file_mtime    INTEGER NOT NULL,
            is_builtin    INTEGER DEFAULT 0,
            enabled       INTEGER DEFAULT 0,
            duplicate_of  TEXT,
            indexed_at    INTEGER,
            scan_version  INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS assets (
            id            TEXT PRIMARY KEY,
            pack_id       TEXT NOT NULL REFERENCES packs(id),
            res_path      TEXT NOT NULL,
            category      TEXT NOT NULL,
            subpath       TEXT,
            file_name     TEXT NOT NULL,
            width         INTEGER NOT NULL,
            height        INTEGER NOT NULL,
            grid_w        REAL,
            grid_h        REAL,
            has_alpha     INTEGER,
            alpha_coverage REAL,
            mean_rgb      TEXT,
            palette       TEXT,
            thumb_path    TEXT,
            pack_tags     TEXT,
            pack_sets     TEXT,
            content_hash  TEXT NOT NULL,
            state         TEXT DEFAULT 'ok',
            last_seen_at  INTEGER,
            UNIQUE (pack_id, res_path)
        );

        CREATE TABLE IF NOT EXISTS enrichment (
            content_hash  TEXT PRIMARY KEY,
            description   TEXT NOT NULL,
            object_kind   TEXT,
            semantic_tags TEXT NOT NULL,
            style_tags    TEXT NOT NULL,
            setting_tags  TEXT,
            dominant_hue  TEXT,
            footprint     TEXT,
            confidence    REAL,
            model         TEXT NOT NULL,
            prompt_version INTEGER NOT NULL,
            created_at    INTEGER
        );

        CREATE TABLE IF NOT EXISTS generated_props (
            id            TEXT PRIMARY KEY,
            name          TEXT NOT NULL,
            description   TEXT NOT NULL,
            style_id      TEXT,
            png_path      TEXT NOT NULL,
            grid_w        REAL,
            grid_h        REAL,
            backend       TEXT,
            seed          INTEGER,
            packed_into   TEXT,
            created_at    INTEGER
        );

        CREATE INDEX IF NOT EXISTS idx_assets_category ON assets(category);
        CREATE INDEX IF NOT EXISTS idx_assets_pack_id ON assets(pack_id);
        CREATE INDEX IF NOT EXISTS idx_assets_content_hash ON assets(content_hash);
        CREATE INDEX IF NOT EXISTS idx_assets_state ON assets(state);
        CREATE INDEX IF NOT EXISTS idx_enrichment_object_kind ON enrichment(object_kind);
        CREATE INDEX IF NOT EXISTS idx_enrichment_footprint ON enrichment(footprint);
        """)

        cur = conn.cursor()
        cur.execute("SELECT version FROM schema_version LIMIT 1;")
        row = cur.fetchone()
        if not row:
            cur.execute("INSERT INTO schema_version (version) VALUES (?);", (SCHEMA_VERSION,))

    return conn


def compute_asset_id(pack_id: str, res_path: str) -> str:
    """Deterministic primary key for an asset row: sha256(pack_id + ':' + res_path)."""
    return hashlib.sha256(f"{pack_id}:{res_path}".encode("utf-8")).hexdigest()


def analyze_image(img: Image.Image) -> Dict[str, Any]:
    """Measure image dimensions, alpha coverage, mean color, and content hash."""
    w, h = img.size
    grid_w = round(w / 256.0, 3)
    grid_h = round(h / 256.0, 3)

    img_rgba = img.convert("RGBA")
    raw_bytes = img_rgba.tobytes()
    # Content hash of the decoded image bytes
    content_hash = hashlib.sha256(raw_bytes).hexdigest()

    if HAVE_NUMPY:
        arr = np.frombuffer(raw_bytes, dtype=np.uint8).reshape((h, w, 4))
        alpha = arr[:, :, 3]
        visible_mask = alpha > 16
        visible_count = int(np.count_nonzero(visible_mask))
        total_pixels = w * h
        has_alpha = int(visible_count < total_pixels)
        alpha_coverage = round(float(visible_count) / total_pixels, 3)

        if visible_count > 0:
            rgb_visible = arr[:, :, :3][visible_mask]
            mean = rgb_visible.mean(axis=0).astype(int)
            mean_rgb = f"#{mean[0]:02x}{mean[1]:02x}{mean[2]:02x}"
        else:
            mean_rgb = "#808080"
    else:
        # Fallback pure PIL
        alpha = img_rgba.getchannel("A")
        total_pixels = w * h
        visible_count = sum(1 for p in alpha.getdata() if p > 16)
        has_alpha = int(visible_count < total_pixels)
        alpha_coverage = round(float(visible_count) / total_pixels, 3)
        mean_rgb = "#808080"

    # Dominant palette (extract up to 5 colors from thumbnail/downscaled version)
    palette_colors = []
    try:
        small = img_rgba.resize((64, 64), Image.Resampling.NEAREST)
        pal_img = small.convert("RGB").quantize(colors=5)
        pal = pal_img.getpalette()
        if pal:
            for i in range(min(5, len(pal) // 3)):
                r, g, b = pal[i * 3:(i + 1) * 3]
                palette_colors.append(f"#{r:02x}{g:02x}{b:02x}")
    except Exception:
        palette_colors = []

    return {
        "width": w,
        "height": h,
        "grid_w": grid_w,
        "grid_h": grid_h,
        "has_alpha": has_alpha,
        "alpha_coverage": alpha_coverage,
        "mean_rgb": mean_rgb,
        "palette": json.dumps(palette_colors),
        "content_hash": content_hash,
    }


def save_thumbnail(img: Image.Image, content_hash: str, thumbs_dir: Optional[Path] = None, target_size: int = 512) -> str:
    """Generate standardized square thumbnail on mid-grey background.

    Saves as WebP in data/thumbnails/<hash[:2]>/<hash>.webp and returns relative path.
    """
    base_dir = Path(thumbs_dir or THUMBS_DIR_DEFAULT)
    sub_dir = base_dir / content_hash[:2]
    sub_dir.mkdir(parents=True, exist_ok=True)
    out_file = sub_dir / f"{content_hash}.webp"

    rel_path = f"{content_hash[:2]}/{content_hash}.webp"
    if out_file.exists():
        return rel_path

    # Standardize image:
    img_rgba = img.convert("RGBA")
    w, h = img_rgba.size

    scale = min(target_size / w, target_size / h)
    new_w = max(1, int(round(w * scale)))
    new_h = max(1, int(round(h * scale)))

    if (new_w, new_h) != (w, h):
        resample = Image.Resampling.LANCZOS if scale < 1.0 else Image.Resampling.BICUBIC
        scaled = img_rgba.resize((new_w, new_h), resample)
    else:
        scaled = img_rgba

    # Composite onto square mid-grey background (#808080)
    bg = Image.new("RGBA", (target_size, target_size), (128, 128, 128, 255))
    paste_x = (target_size - new_w) // 2
    paste_y = (target_size - new_h) // 2
    bg.paste(scaled, (paste_x, paste_y), scaled)

    bg.convert("RGB").save(str(out_file), "WEBP", quality=90)
    return rel_path


class AssetDatabase:
    """High-level database access object for assets.db."""

    def __init__(self, db_path: Optional[Path] = None, thumbs_dir: Optional[Path] = None):
        self.db_path = Path(db_path or DB_PATH_DEFAULT)
        self.thumbs_dir = Path(thumbs_dir or THUMBS_DIR_DEFAULT)
        self.conn = init_db(self.db_path)

    def close(self):
        self.conn.close()

    def get_packs(self) -> Dict[str, dict]:
        """Return dict of pack_id -> pack_dict."""
        cur = self.conn.cursor()
        cur.execute("SELECT id, name, author, version, file_path, file_size, file_mtime, is_builtin, enabled, duplicate_of, indexed_at, scan_version FROM packs;")
        cols = [col[0] for col in cur.description]
        return {row[0]: dict(zip(cols, row)) for row in cur.fetchall()}

    def upsert_pack(self, pack_dict: dict):
        """Insert or update pack metadata."""
        with self.conn:
            self.conn.execute("""
            INSERT INTO packs (
                id, name, author, version, file_path, file_size, file_mtime,
                is_builtin, enabled, duplicate_of, indexed_at, scan_version
            ) VALUES (
                :id, :name, :author, :version, :file_path, :file_size, :file_mtime,
                :is_builtin, :enabled, :duplicate_of, :indexed_at, :scan_version
            )
            ON CONFLICT(id) DO UPDATE SET
                name = excluded.name,
                author = excluded.author,
                version = excluded.version,
                file_path = excluded.file_path,
                file_size = excluded.file_size,
                file_mtime = excluded.file_mtime,
                enabled = excluded.enabled,
                duplicate_of = excluded.duplicate_of,
                indexed_at = excluded.indexed_at,
                scan_version = excluded.scan_version;
            """, pack_dict)

    def upsert_asset(self, asset_dict: dict):
        """Insert or update asset metadata."""
        with self.conn:
            self.conn.execute("""
            INSERT INTO assets (
                id, pack_id, res_path, category, subpath, file_name,
                width, height, grid_w, grid_h, has_alpha, alpha_coverage,
                mean_rgb, palette, thumb_path, pack_tags, pack_sets,
                content_hash, state, last_seen_at
            ) VALUES (
                :id, :pack_id, :res_path, :category, :subpath, :file_name,
                :width, :height, :grid_w, :grid_h, :has_alpha, :alpha_coverage,
                :mean_rgb, :palette, :thumb_path, :pack_tags, :pack_sets,
                :content_hash, :state, :last_seen_at
            )
            ON CONFLICT(pack_id, res_path) DO UPDATE SET
                category = excluded.category,
                subpath = excluded.subpath,
                file_name = excluded.file_name,
                width = excluded.width,
                height = excluded.height,
                grid_w = excluded.grid_w,
                grid_h = excluded.grid_h,
                has_alpha = excluded.has_alpha,
                alpha_coverage = excluded.alpha_coverage,
                mean_rgb = excluded.mean_rgb,
                palette = excluded.palette,
                thumb_path = excluded.thumb_path,
                pack_tags = excluded.pack_tags,
                pack_sets = excluded.pack_sets,
                content_hash = excluded.content_hash,
                state = excluded.state,
                last_seen_at = excluded.last_seen_at;
            """, asset_dict)

    def upsert_assets_batch(self, asset_dicts: List[dict]):
        """Batch insert or update asset metadata in a single transaction."""
        if not asset_dicts:
            return
        with self.conn:
            self.conn.executemany("""
            INSERT INTO assets (
                id, pack_id, res_path, category, subpath, file_name,
                width, height, grid_w, grid_h, has_alpha, alpha_coverage,
                mean_rgb, palette, thumb_path, pack_tags, pack_sets,
                content_hash, state, last_seen_at
            ) VALUES (
                :id, :pack_id, :res_path, :category, :subpath, :file_name,
                :width, :height, :grid_w, :grid_h, :has_alpha, :alpha_coverage,
                :mean_rgb, :palette, :thumb_path, :pack_tags, :pack_sets,
                :content_hash, :state, :last_seen_at
            )
            ON CONFLICT(pack_id, res_path) DO UPDATE SET
                category = excluded.category,
                subpath = excluded.subpath,
                file_name = excluded.file_name,
                width = excluded.width,
                height = excluded.height,
                grid_w = excluded.grid_w,
                grid_h = excluded.grid_h,
                has_alpha = excluded.has_alpha,
                alpha_coverage = excluded.alpha_coverage,
                mean_rgb = excluded.mean_rgb,
                palette = excluded.palette,
                thumb_path = excluded.thumb_path,
                pack_tags = excluded.pack_tags,
                pack_sets = excluded.pack_sets,
                content_hash = excluded.content_hash,
                state = excluded.state,
                last_seen_at = excluded.last_seen_at;
            """, asset_dicts)

    def upsert_enrichment(self, enr_dict: dict):
        """Insert or update enrichment description keyed by content_hash."""
        with self.conn:
            self.conn.execute("""
            INSERT INTO enrichment (
                content_hash, description, object_kind, semantic_tags, style_tags,
                setting_tags, dominant_hue, footprint, confidence, model,
                prompt_version, created_at
            ) VALUES (
                :content_hash, :description, :object_kind, :semantic_tags, :style_tags,
                :setting_tags, :dominant_hue, :footprint, :confidence, :model,
                :prompt_version, :created_at
            )
            ON CONFLICT(content_hash) DO UPDATE SET
                description = excluded.description,
                object_kind = excluded.object_kind,
                semantic_tags = excluded.semantic_tags,
                style_tags = excluded.style_tags,
                setting_tags = excluded.setting_tags,
                dominant_hue = excluded.dominant_hue,
                footprint = excluded.footprint,
                confidence = excluded.confidence,
                model = excluded.model,
                prompt_version = excluded.prompt_version,
                created_at = excluded.created_at;
            """, enr_dict)

    def upsert_generated_prop(self, prop_dict: dict):
        """Insert or update a generated prop record."""
        with self.conn:
            self.conn.execute("""
            INSERT INTO generated_props (
                id, name, description, style_id, png_path, grid_w, grid_h,
                backend, seed, packed_into, created_at
            ) VALUES (
                :id, :name, :description, :style_id, :png_path, :grid_w, :grid_h,
                :backend, :seed, :packed_into, :created_at
            )
            ON CONFLICT(id) DO UPDATE SET
                description = excluded.description,
                style_id = excluded.style_id,
                png_path = excluded.png_path,
                grid_w = excluded.grid_w,
                grid_h = excluded.grid_h,
                backend = excluded.backend,
                seed = excluded.seed,
                packed_into = excluded.packed_into,
                created_at = excluded.created_at;
            """, prop_dict)

    def get_stats(self) -> Dict[str, Any]:
        """Aggregate database metrics."""
        cur = self.conn.cursor()
        cur.execute("SELECT COUNT(*) FROM packs;")
        pack_count = cur.fetchone()[0]

        cur.execute("SELECT COUNT(*) FROM packs WHERE enabled = 1;")
        enabled_packs = cur.fetchone()[0]

        cur.execute("SELECT COUNT(*) FROM assets WHERE state = 'ok';")
        asset_count = cur.fetchone()[0]

        cur.execute("SELECT category, COUNT(*) FROM assets WHERE state = 'ok' GROUP BY category;")
        categories = dict(cur.fetchall())

        cur.execute("SELECT COUNT(DISTINCT content_hash) FROM assets WHERE state = 'ok';")
        unique_hashes = cur.fetchone()[0]

        cur.execute("SELECT COUNT(*) FROM enrichment;")
        enriched_count = cur.fetchone()[0]

        cur.execute("SELECT COUNT(*) FROM assets a JOIN packs p ON a.pack_id = p.id WHERE a.state = 'ok' AND p.is_builtin = 1;")
        stock_assets = cur.fetchone()[0]

        cur.execute("SELECT COUNT(*) FROM assets a JOIN packs p ON a.pack_id = p.id WHERE a.state = 'ok' AND p.is_builtin = 0;")
        custom_assets = cur.fetchone()[0]

        cur.execute("SELECT COUNT(DISTINCT a.content_hash) FROM assets a JOIN packs p ON a.pack_id = p.id JOIN enrichment e ON a.content_hash = e.content_hash WHERE a.state = 'ok' AND p.is_builtin = 1;")
        stock_enriched = cur.fetchone()[0]

        cur.execute("SELECT COUNT(DISTINCT a.content_hash) FROM assets a JOIN packs p ON a.pack_id = p.id JOIN enrichment e ON a.content_hash = e.content_hash WHERE a.state = 'ok' AND p.is_builtin = 0;")
        custom_enriched = cur.fetchone()[0]

        return {
            "packs": pack_count,
            "enabled_packs": enabled_packs,
            "assets": asset_count,
            "stock_assets": stock_assets,
            "custom_assets": custom_assets,
            "unique_textures": unique_hashes,
            "enriched": enriched_count,
            "stock_enriched": stock_enriched,
            "custom_enriched": custom_enriched,
            "categories": categories,
        }
