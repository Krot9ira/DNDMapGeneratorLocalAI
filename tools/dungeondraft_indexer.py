#!/usr/bin/env python3
"""Dungeondraft Asset Indexer.

Scans Dungeondraft builtin assets (Dungeondraft.pck) and custom asset packs
(.dungeondraft_pack), extracts metadata and standardized thumbnails, and indexes
everything into SQLite (data/assets.db).
"""
import argparse
import concurrent.futures
import io
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (ValueError, OSError):
        pass

from paths import ROOT as PROJECT_ROOT
from dungeondraft_pck import PckReader, PckError
from dungeondraft_db import (
    AssetDatabase,
    analyze_image,
    compute_asset_id,
    save_thumbnail,
    DB_PATH_DEFAULT,
    THUMBS_DIR_DEFAULT,
)

from dungeondraft_enrich import PROMPT_VERSION

DEFAULT_WORKERS = min(32, max(4, (os.cpu_count() or 4)))


def _process_texture_item(
    reader: PckReader,
    item: Tuple[str, str, Optional[str], str],
    pack_id: str,
    thumbs_dir: Path,
    tags_by_rel_path: Optional[Dict[str, List[str]]] = None,
    sets_by_rel_path: Optional[Dict[str, List[str]]] = None,
) -> Optional[dict]:
    """Process a single texture (extract, analyze, thumbnail, prepare asset row). Thread-safe."""
    res_path, category, subpath, file_name = item
    try:
        img = reader.extract_image(res_path)
        if img is None:
            return None

        metrics = analyze_image(img)
        thumb_rel = save_thumbnail(img, metrics["content_hash"], thumbs_dir)

        p_tags: List[str] = []
        p_sets: List[str] = []
        if tags_by_rel_path:
            rel_to_pack = ""
            if "/textures/" in res_path:
                rel_to_pack = "textures/" + res_path.split("/textures/")[1]
            p_tags = tags_by_rel_path.get(rel_to_pack, [])
            if sets_by_rel_path:
                p_sets = sets_by_rel_path.get(rel_to_pack, [])

        asset_id = compute_asset_id(pack_id, res_path)
        return {
            "id": asset_id,
            "pack_id": pack_id,
            "res_path": res_path,
            "category": category,
            "subpath": subpath,
            "file_name": file_name,
            "width": metrics["width"],
            "height": metrics["height"],
            "grid_w": metrics["grid_w"],
            "grid_h": metrics["grid_h"],
            "has_alpha": metrics["has_alpha"],
            "alpha_coverage": metrics["alpha_coverage"],
            "mean_rgb": metrics["mean_rgb"],
            "palette": metrics["palette"],
            "thumb_path": thumb_rel,
            "pack_tags": json.dumps(p_tags),
            "pack_sets": json.dumps(p_sets),
            "content_hash": metrics["content_hash"],
            "state": "ok",
            "last_seen_at": int(time.time()),
        }
    except Exception:
        return None


def index_textures_parallel(
    db: AssetDatabase,
    reader: PckReader,
    textures: List[Tuple[str, str, Optional[str], str]],
    pack_id: str,
    tags_by_rel_path: Optional[Dict[str, List[str]]] = None,
    sets_by_rel_path: Optional[Dict[str, List[str]]] = None,
    max_workers: int = DEFAULT_WORKERS,
    batch_size: int = 500,
    progress_label: str = "",
) -> Tuple[int, int]:
    """Index a list of textures in parallel using threads, writing to DB in batches."""
    total = len(textures)
    if total == 0:
        return 0, 0

    indexed = 0
    skipped = 0
    batch: List[dict] = []

    def task(item):
        return _process_texture_item(reader, item, pack_id, db.thumbs_dir, tags_by_rel_path, sets_by_rel_path)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
        for i, res in enumerate(executor.map(task, textures), 1):
            if res is not None:
                batch.append(res)
                indexed += 1
            else:
                skipped += 1

            if len(batch) >= batch_size:
                db.upsert_assets_batch(batch)
                batch.clear()

            if i % 1000 == 0 or i == total:
                prefix = f"  [{progress_label}] " if progress_label else "  "
                print(f"{prefix}Indexed {i}/{total} textures ({indexed} ok, {skipped} skipped)...", flush=True)

    if batch:
        db.upsert_assets_batch(batch)
        batch.clear()

    return indexed, skipped

SCAN_VERSION = 1
DEFAULT_CONFIG_PATH = Path(os.path.expandvars(r"%APPDATA%\Dungeondraft\config.ini"))
DEFAULT_INSTALL_DIR = Path(r"D:\programs\dungeondraft\Dungeondraft")


def find_dungeondraft_config() -> Optional[Path]:
    """Find Dungeondraft config.ini path."""
    if DEFAULT_CONFIG_PATH.exists():
        return DEFAULT_CONFIG_PATH
    # Alternate locations on Linux/Mac or custom wine prefixes
    home = Path.home()
    alt_paths = [
        home / ".config" / "Dungeondraft" / "config.ini",
        home / "AppData" / "Roaming" / "Dungeondraft" / "config.ini",
    ]
    for p in alt_paths:
        if p.exists():
            return p
    return None


def read_dungeondraft_config(config_path: Optional[Path] = None) -> dict:
    """Parse Dungeondraft config.ini settings."""
    path = config_path or find_dungeondraft_config()
    if not path or not path.exists():
        return {
            "custom_assets_directory": None,
            "active_asset_packs": [],
            "disable_default_assets": False,
        }

    text = path.read_text(encoding="utf-8", errors="replace")

    # custom_assets_directory
    cad_match = re.search(r'custom_assets_directory\s*=\s*"([^"]+)"', text)
    custom_dir = cad_match.group(1).replace("\\\\", "\\") if cad_match else None

    # active_asset_packs array
    active_packs = []
    aap_match = re.search(r'active_asset_packs\s*=\s*(\[[^\]]*\])', text, re.DOTALL)
    if aap_match:
        try:
            active_packs = json.loads(aap_match.group(1))
        except Exception:
            # Regex fallback
            active_packs = re.findall(r'"([^"]+)"', aap_match.group(1))

    # disable_default_assets
    dda_match = re.search(r'disable_default_assets\s*=\s*(true|false)', text, re.IGNORECASE)
    disable_default = dda_match.group(1).lower() == "true" if dda_match else False

    return {
        "custom_assets_directory": custom_dir,
        "active_asset_packs": active_packs,
        "disable_default_assets": disable_default,
    }


def find_dungeondraft_pck() -> Optional[Path]:
    """Locate Dungeondraft.pck."""
    candidates = [
        DEFAULT_INSTALL_DIR / "Dungeondraft.pck",
        Path(r"C:\Program Files\Dungeondraft\Dungeondraft.pck"),
        Path(r"C:\Program Files (x86)\Dungeondraft\Dungeondraft.pck"),
        Path(r"D:\Dungeondraft\Dungeondraft.pck"),
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


class DungeondraftIndexer:
    """Indexes stock and custom Dungeondraft asset packs into SQLite."""

    def __init__(
        self,
        db_path: Optional[Path] = None,
        thumbs_dir: Optional[Path] = None,
        max_workers: int = DEFAULT_WORKERS,
    ):
        self.db = AssetDatabase(db_path=db_path, thumbs_dir=thumbs_dir)
        self.max_workers = max_workers

    def close(self):
        self.db.close()

    def index_builtin(self, pck_path: Optional[Path] = None) -> Tuple[int, int]:
        """Index built-in textures from Dungeondraft.pck. Returns (assets_indexed, skipped)."""
        path = pck_path or find_dungeondraft_pck()
        if not path or not path.exists():
            print(f"Warning: Dungeondraft.pck not found at {path}")
            return 0, 0

        pck_file = os.path.abspath(str(path))
        print(f"Indexing built-in assets from {pck_file} (using {self.max_workers} worker threads)...")

        reader = PckReader(pck_file)
        stat = os.stat(pck_file)
        pack_dict = {
            "id": "default",
            "name": "Dungeondraft Standard Assets",
            "author": "Megasploot",
            "version": "1.0",
            "file_path": pck_file,
            "file_size": stat.st_size,
            "file_mtime": int(stat.st_mtime),
            "is_builtin": 1,
            "enabled": 1,
            "duplicate_of": None,
            "indexed_at": int(time.time()),
            "scan_version": SCAN_VERSION,
        }
        self.db.upsert_pack(pack_dict)

        textures = reader.list_textures()
        indexed, skipped = index_textures_parallel(
            db=self.db,
            reader=reader,
            textures=textures,
            pack_id="default",
            max_workers=self.max_workers,
            progress_label="Built-in",
        )

        print(f"Built-in indexing complete: {indexed} assets indexed, {skipped} skipped.")
        return indexed, skipped

    def scan_all(
        self,
        assets_dir: Optional[str] = None,
        active_pack_ids: Optional[List[str]] = None,
        include_builtin: bool = True,
        builtin_pck: Optional[Path] = None,
        enabled_only: bool = True,
    ) -> Dict[str, Any]:
        """Scan configured assets directory and index all valid packs."""
        config = read_dungeondraft_config()
        scan_dir = assets_dir or config.get("custom_assets_directory")
        active_ids = set(active_pack_ids or config.get("active_asset_packs") or [])
        disable_default = config.get("disable_default_assets", False)

        results = {
            "builtin_indexed": 0,
            "packs_found": 0,
            "packs_indexed": 0,
            "packs_disallowed": 0,
            "packs_duplicate": 0,
            "assets_indexed": 0,
            "assets_skipped": 0,
            "missing_enabled_packs": [],
        }

        # Step 1: Builtin assets
        if include_builtin and not disable_default:
            b_idx, b_skp = self.index_builtin(builtin_pck)
            results["builtin_indexed"] = b_idx

        if not scan_dir or not os.path.exists(scan_dir):
            print(f"Custom assets directory not found: {scan_dir}")
            return results

        print(f"Scanning custom packs in {scan_dir} (using {self.max_workers} worker threads)...")
        pack_files = []
        for root, _, files in os.walk(scan_dir):
            for f in files:
                if f.lower().endswith(".dungeondraft_pack"):
                    pack_files.append(os.path.join(root, f))

        results["packs_found"] = len(pack_files)
        print(f"Found {len(pack_files)} pack files on disk. Checking manifests...")

        seen_pack_ids: Dict[str, str] = {}  # pack_id -> primary_file_path
        found_pack_ids: Set[str] = set()

        for idx, pack_path in enumerate(pack_files, 1):
            try:
                reader = PckReader(pack_path)
            except PckError as exc:
                print(f"  Warning: Cannot read pack {os.path.basename(pack_path)}: {exc}")
                continue

            pack_id = reader.pack_id
            if not pack_id:
                print(f"  Warning: Pack has no ID in pack.json: {os.path.basename(pack_path)}")
                continue

            found_pack_ids.add(pack_id)
            is_enabled = 1 if (pack_id in active_ids) else 0

            # If user requested enabled_only and this pack is disabled, we record the pack row but skip textures
            if enabled_only and not is_enabled:
                stat = os.stat(pack_path)
                pack_name = (reader.pack_json or {}).get("name", os.path.basename(pack_path))
                pack_dict = {
                    "id": pack_id,
                    "name": pack_name,
                    "author": (reader.pack_json or {}).get("author", ""),
                    "version": str((reader.pack_json or {}).get("version", "")),
                    "file_path": os.path.abspath(pack_path),
                    "file_size": stat.st_size,
                    "file_mtime": int(stat.st_mtime),
                    "is_builtin": 0,
                    "enabled": 0,
                    "duplicate_of": None,
                    "indexed_at": int(time.time()),
                    "scan_version": SCAN_VERSION,
                }
                self.db.upsert_pack(pack_dict)
                continue

            # Check if this is a duplicate file for an already seen pack ID
            duplicate_of = None
            if pack_id in seen_pack_ids:
                duplicate_of = seen_pack_ids[pack_id]
                results["packs_duplicate"] += 1
                print(f"  [Duplicate] Pack ID {pack_id} in {os.path.basename(pack_path)} (duplicate of {os.path.basename(duplicate_of)})")
            else:
                seen_pack_ids[pack_id] = pack_path

            # Check license: allow_3rd_party_mapping_software_to_read
            if not reader.is_allowed_for_third_party():
                results["packs_disallowed"] += 1
                pack_name = (reader.pack_json or {}).get("name", os.path.basename(pack_path))
                print(f"  [Skipped - License] Pack '{pack_name}' ({pack_id}) disallows 3rd party mapping software.")
                stat = os.stat(pack_path)
                pack_dict = {
                    "id": pack_id,
                    "name": pack_name,
                    "author": (reader.pack_json or {}).get("author", ""),
                    "version": str((reader.pack_json or {}).get("version", "")),
                    "file_path": os.path.abspath(pack_path),
                    "file_size": stat.st_size,
                    "file_mtime": int(stat.st_mtime),
                    "is_builtin": 0,
                    "enabled": is_enabled,
                    "duplicate_of": duplicate_of,
                    "indexed_at": int(time.time()),
                    "scan_version": SCAN_VERSION,
                }
                self.db.upsert_pack(pack_dict)
                continue

            # Index this pack's textures if not a duplicate
            stat = os.stat(pack_path)
            pack_name = (reader.pack_json or {}).get("name", os.path.basename(pack_path))
            pack_dict = {
                "id": pack_id,
                "name": pack_name,
                "author": (reader.pack_json or {}).get("author", ""),
                "version": str((reader.pack_json or {}).get("version", "")),
                "file_path": os.path.abspath(pack_path),
                "file_size": stat.st_size,
                "file_mtime": int(stat.st_mtime),
                "is_builtin": 0,
                "enabled": is_enabled,
                "duplicate_of": duplicate_of,
                "indexed_at": int(time.time()),
                "scan_version": SCAN_VERSION,
            }
            self.db.upsert_pack(pack_dict)

            if duplicate_of:
                # Do not re-index duplicate textures
                continue

            results["packs_indexed"] += 1

            # Prepare tag lookup for this pack
            tags_by_rel_path: Dict[str, List[str]] = {}
            sets_by_rel_path: Dict[str, List[str]] = {}
            if reader.tags_data:
                tag_dict = reader.tags_data.get("tags", {})
                set_dict = reader.tags_data.get("sets", {})
                for tag_name, path_list in tag_dict.items():
                    for tp in path_list:
                        clean_tp = tp.replace("\\", "/").lstrip("/")
                        tags_by_rel_path.setdefault(clean_tp, []).append(tag_name)
                for set_name, tag_list in set_dict.items():
                    for tname in tag_list:
                        for tp in tag_dict.get(tname, []):
                            clean_tp = tp.replace("\\", "/").lstrip("/")
                            sets_by_rel_path.setdefault(clean_tp, []).append(set_name)

            textures = reader.list_textures()
            tex_total = len(textures)

            print(f"  [{idx}/{len(pack_files)}] Indexing '{pack_name}' ({tex_total} textures)...")
            tex_ok, tex_skip = index_textures_parallel(
                db=self.db,
                reader=reader,
                textures=textures,
                pack_id=pack_id,
                tags_by_rel_path=tags_by_rel_path,
                sets_by_rel_path=sets_by_rel_path,
                max_workers=self.max_workers,
                progress_label=f"{idx}/{len(pack_files)} {pack_name[:20]}",
            )
            results["assets_indexed"] += tex_ok
            results["assets_skipped"] += tex_skip

        # Check for missing active packs
        missing_packs = [pid for pid in active_ids if pid not in found_pack_ids]
        results["missing_enabled_packs"] = missing_packs
        if missing_packs:
            print(f"Warning: {len(missing_packs)} active pack IDs in config.ini not found on disk: {missing_packs}")

        # Mark disappeared custom packs as disabled or gone in DB
        with self.db.conn:
            for pid, pdata in self.db.get_packs().items():
                if not pdata.get("is_builtin"):
                    if pid not in found_pack_ids:
                        self.db.conn.execute("UPDATE packs SET enabled = 0 WHERE id = ?;", (pid,))
                        self.db.conn.execute("UPDATE assets SET state = 'gone' WHERE pack_id = ?;", (pid,))

        return results

    def validate(self, assets_dir=None) -> Dict[str, Any]:
        """Compare the index against the packs on disk and the enrichment it has.

        This is what keeps a catalogue honest across a Dungeondraft update or a
        pack the user re-downloaded: it says what is new, gone, redrawn and
        stale, so re-cataloguing can touch only that list instead of all of it.
        Nothing is written - it reports, the user decides.
        """
        config = read_dungeondraft_config()
        scan_dir = assets_dir or config.get("custom_assets_directory")
        active_ids = set(config.get("active_asset_packs") or [])

        cur = self.db.conn.cursor()
        cur.execute("SELECT id, name, file_path, file_size, file_mtime, enabled, is_builtin FROM packs;")
        indexed = {r["id"]: dict(r) for r in cur.fetchall()}

        result: Dict[str, Any] = {
            "new_packs": [], "missing_packs": [], "changed_packs": [],
            "newly_enabled": [], "newly_disabled": [], "licence_skipped": [],
            "unreadable": [],
        }

        # Built-in assets travel with Dungeondraft itself, not with the pack folder.
        for pid, row in indexed.items():
            if not row["is_builtin"]:
                continue
            if not os.path.exists(row["file_path"]):
                result["missing_packs"].append((pid, row["name"]))
                continue
            stat = os.stat(row["file_path"])
            if stat.st_size != row["file_size"] or int(stat.st_mtime) != row["file_mtime"]:
                result["changed_packs"].append((pid, row["name"], "Dungeondraft was updated"))

        on_disk = {}
        if scan_dir and os.path.exists(scan_dir):
            for root, _, files in os.walk(scan_dir):
                for f in files:
                    if not f.lower().endswith(".dungeondraft_pack"):
                        continue
                    path = os.path.join(root, f)
                    try:
                        reader = PckReader(path)
                    except PckError as exc:
                        result["unreadable"].append((os.path.basename(path), str(exc)))
                        continue
                    if not reader.pack_id:
                        result["unreadable"].append((os.path.basename(path), "no id in pack.json"))
                        continue
                    if not reader.is_allowed_for_third_party():
                        result["licence_skipped"].append((reader.pack_id, os.path.basename(path)))
                        continue
                    # 20 pack ids ship in more than one file. Keep every copy:
                    # comparing the index against whichever copy os.walk reached
                    # first reports twenty packs as redrawn every single time.
                    on_disk.setdefault(reader.pack_id, []).append(path)
        else:
            print(f"Warning: custom assets directory not found: {scan_dir}")

        for pid, paths in on_disk.items():
            row = indexed.get(pid)
            if not row:
                result["new_packs"].append((pid, os.path.basename(paths[0])))
                continue
            # Compare the file the index actually read, not a namesake of it.
            path = row["file_path"] if row["file_path"] in paths else None
            if path is None:
                result["changed_packs"].append((pid, row["name"], "the indexed file moved or was replaced"))
            else:
                stat = os.stat(path)
                if stat.st_size != row["file_size"] or int(stat.st_mtime) != row["file_mtime"]:
                    result["changed_packs"].append((pid, row["name"], "file on disk differs"))
            enabled_now = 1 if pid in active_ids else 0
            if enabled_now and not row["enabled"]:
                result["newly_enabled"].append((pid, row["name"]))
            elif row["enabled"] and not enabled_now:
                result["newly_disabled"].append((pid, row["name"]))

        for pid, row in indexed.items():
            if row["is_builtin"] or pid in on_disk:
                continue
            if not os.path.exists(row["file_path"]):
                result["missing_packs"].append((pid, row["name"]))

        # Asset-level drift, but only inside packs whose file changed. An
        # untouched pack file cannot contain a redrawn texture, so hashing all
        # 250 000 of them to prove it would be an expensive way to learn nothing.
        result["assets_new"] = []
        result["assets_gone"] = []
        result["assets_redrawn"] = []
        result["assets_resized"] = []
        for pid, name, _why in result["changed_packs"]:
            row = indexed.get(pid)
            path = row["file_path"] if row else None
            if not path or not os.path.exists(path):
                continue
            try:
                reader = PckReader(path)
            except PckError:
                continue

            cur.execute("SELECT res_path, content_hash, width, height FROM assets WHERE pack_id = ?;", (pid,))
            known = {r["res_path"]: dict(r) for r in cur.fetchall()}
            on_pack = {t[0] for t in reader.list_textures()}

            for res_path in sorted(on_pack - set(known)):
                result["assets_new"].append((name, res_path))
            for res_path in sorted(set(known) - on_pack):
                result["assets_gone"].append((name, res_path))

            common_paths = sorted(on_pack & set(known))

            def check_tex(res_path):
                try:
                    img = reader.extract_image(res_path)
                    if img is None:
                        return res_path, None
                    metrics = analyze_image(img)
                    return res_path, metrics
                except Exception:
                    return res_path, None

            with concurrent.futures.ThreadPoolExecutor(max_workers=self.max_workers) as executor:
                for res_path, metrics in executor.map(check_tex, common_paths):
                    if metrics is None:
                        continue
                    was = known[res_path]
                    if metrics["content_hash"] != was["content_hash"]:
                        result["assets_redrawn"].append((name, res_path))
                    if metrics["width"] != was["width"] or metrics["height"] != was["height"]:
                        result["assets_resized"].append((name, res_path))

        # Enrichment: what is missing, what was written by an older prompt, and
        # what describes a texture no pack contains any more.
        cur.execute("SELECT COUNT(*) FROM assets a LEFT JOIN enrichment e ON a.content_hash = e.content_hash "
                    "WHERE a.state = 'ok' AND e.content_hash IS NULL;")
        result["assets_unenriched"] = cur.fetchone()[0]
        cur.execute("SELECT COUNT(*) FROM enrichment WHERE prompt_version < ?;", (PROMPT_VERSION,))
        result["enrichment_stale"] = cur.fetchone()[0]
        cur.execute("SELECT COUNT(*) FROM enrichment e WHERE NOT EXISTS "
                    "(SELECT 1 FROM assets a WHERE a.content_hash = e.content_hash);")
        result["enrichment_orphaned"] = cur.fetchone()[0]
        cur.execute("SELECT COUNT(*) FROM assets WHERE state = 'ok' AND (thumb_path IS NULL OR thumb_path = '');")
        result["assets_without_thumbnail"] = cur.fetchone()[0]

        # Suspect enrichment detection (unclear, low confidence, empty tags/description)
        cur.execute("""
            SELECT COUNT(*) FROM enrichment
            WHERE object_kind = 'unclear'
               OR confidence < 0.5
               OR semantic_tags = '[]'
               OR semantic_tags = ''
               OR description = '';
        """)
        result["enrichment_suspect"] = cur.fetchone()[0]

        # Duplicate descriptions across different content hashes
        cur.execute("""
            SELECT COUNT(*) FROM (
                SELECT description FROM enrichment
                WHERE description != '' AND description != 'unclear'
                GROUP BY description
                HAVING COUNT(content_hash) > 1
            );
        """)
        result["enrichment_duplicate_descriptions"] = cur.fetchone()[0]

        return result


def print_validation(result: Dict[str, Any]) -> bool:
    """Print a validation result. Returns True when the index needs no work."""

    def section(title, rows, fmt):
        if not rows:
            return 0
        print(f"\n{title} ({len(rows)}):")
        for r in rows[:20]:
            print(f"  {fmt(r)}")
        if len(rows) > 20:
            print(f"  ... and {len(rows) - 20} more")
        return len(rows)

    drift = 0
    drift += section("NEW - on disk, never indexed", result["new_packs"], lambda r: f"{r[1]} [{r[0]}]")
    drift += section("GONE - indexed, no longer on disk", result["missing_packs"], lambda r: f"{r[1]} [{r[0]}]")
    drift += section("REDRAWN - the file changed since it was indexed", result["changed_packs"],
                     lambda r: f"{r[1]} [{r[0]}] - {r[2]}")
    drift += section("NEWLY ENABLED - turned on in config.ini since the scan", result["newly_enabled"],
                     lambda r: f"{r[1]} [{r[0]}]")
    section("NEWLY DISABLED - turned off in config.ini since the scan", result["newly_disabled"],
            lambda r: f"{r[1]} [{r[0]}]")
    section("SKIPPED - the pack author disallows third-party mapping software", result["licence_skipped"],
            lambda r: f"{r[1]} [{r[0]}]")
    section("UNREADABLE", result["unreadable"], lambda r: f"{r[0]} - {r[1]}")

    # Inside the packs that changed. "Gone" is the dangerous one: a map already
    # written against that path will not open cleanly.
    asset_fmt = lambda r: f"{r[0]}: {r[1]}"
    drift += section("ASSETS GONE - a map that used these will not open cleanly",
                     result["assets_gone"], asset_fmt)
    drift += section("ASSETS REDRAWN - same path, different picture; the description may now be a lie",
                     result["assets_redrawn"], asset_fmt)
    drift += section("ASSETS RESIZED - the grid footprint on record is wrong",
                     result["assets_resized"], asset_fmt)
    drift += section("ASSETS NEW - in the pack, not in the catalogue", result["assets_new"], asset_fmt)

    print("\nEnrichment:")
    print(f"  assets with no description       : {result['assets_unenriched']}")
    print(f"  descriptions from an older prompt: {result['enrichment_stale']}")
    print(f"  descriptions with no asset left  : {result['enrichment_orphaned']}")
    print(f"  assets with no thumbnail         : {result['assets_without_thumbnail']}")
    print(f"  suspect descriptions (weak/low)  : {result.get('enrichment_suspect', 0)}")
    print(f"  duplicate description clusters   : {result.get('enrichment_duplicate_descriptions', 0)}")

    drift += result["enrichment_stale"]
    if drift == 0:
        print("\nThe index matches the packs on disk. Nothing to re-scan.")
        return True
    print("\nRe-run 'scan' to bring the index up to date, then catalogue what it adds.")
    return False


def main():
    parser = argparse.ArgumentParser(description="Dungeondraft Asset Indexer CLI")
    subparsers = parser.add_subparsers(dest="command", help="Command to run")

    # scan
    scan_parser = subparsers.add_parser("scan", help="Scan and index asset packs")
    scan_parser.add_argument("--stock-only", action="store_true", help="Index only stock assets from Dungeondraft.pck")
    scan_parser.add_argument("--all-packs", action="store_true", help="Index all packs on disk, not just enabled ones")
    scan_parser.add_argument("--assets-dir", type=str, help="Override custom assets directory")
    scan_parser.add_argument("--pck", type=str, help="Override path to Dungeondraft.pck")
    scan_parser.add_argument("--threads", type=int, default=DEFAULT_WORKERS, help="Number of worker threads")

    # stats
    stats_parser = subparsers.add_parser("stats", help="Display asset database statistics")
    stats_parser.add_argument("--json", action="store_true", help="Output statistics in JSON format")

    # validate
    validate_parser = subparsers.add_parser("validate", help="Validate index against packs on disk")
    validate_parser.add_argument("--assets-dir", type=str, help="Override custom assets directory")
    validate_parser.add_argument("--threads", type=int, default=DEFAULT_WORKERS, help="Number of worker threads")

    args = parser.parse_args()

    if args.command == "scan" or not args.command:
        workers = getattr(args, "threads", DEFAULT_WORKERS)
        indexer = DungeondraftIndexer(max_workers=workers)
        try:
            if getattr(args, "stock_only", False):
                pck_path = Path(args.pck) if args.pck else None
                indexer.index_builtin(pck_path)
            else:
                pck_path = Path(args.pck) if getattr(args, "pck", None) else None
                enabled_only = not getattr(args, "all_packs", False)
                res = indexer.scan_all(
                    assets_dir=getattr(args, "assets_dir", None),
                    include_builtin=True,
                    builtin_pck=pck_path,
                    enabled_only=enabled_only,
                )
                print("\nScan summary:")
                print(json.dumps(res, indent=2))
        finally:
            indexer.close()

    elif args.command == "validate":
        workers = getattr(args, "threads", DEFAULT_WORKERS)
        indexer = DungeondraftIndexer(max_workers=workers)
        try:
            clean = print_validation(indexer.validate(assets_dir=getattr(args, "assets_dir", None)))
        finally:
            indexer.close()
        sys.exit(0 if clean else 1)

    elif args.command == "stats":
        indexer = DungeondraftIndexer()
        try:
            stats = indexer.db.get_stats()
            if getattr(args, "json", False):
                print(json.dumps(stats, indent=2))
            else:
                print("\nDungeondraft Asset Library Statistics:")
                print(f"  Total packs in DB:    {stats['packs']}")
                print(f"  Active/Enabled packs: {stats['enabled_packs']}")
                print(f"  Active assets:        {stats['assets']}")
                print(f"  Unique content hashes:{stats['unique_textures']}")
                print(f"  Enriched assets:      {stats['enriched']}")
                print("\nAssets by Category:")
                for cat, count in sorted(stats["categories"].items(), key=lambda x: -x[1]):
                    print(f"  {cat:<15}: {count:>6}")
        finally:
            indexer.close()


if __name__ == "__main__":
    main()
