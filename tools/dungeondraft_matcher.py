#!/usr/bin/env python3
"""Asset Matcher for Dungeondraft Map Generation.

Maps plan requirements (rooms, terrain, walls, doors, windows, props, lights)
to indexed Dungeondraft assets in assets.db using semantic tags, object kinds,
dimensions, and style hints with deterministic seeding.
"""
import hashlib
import json
import random
import re
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from dungeondraft_db import AssetDatabase, DB_PATH_DEFAULT

# Default fallbacks if database is minimal
DEFAULT_STOCK_WALL = "res://textures/walls/sample_wall.png"
DEFAULT_STOCK_DOOR = "res://textures/portals/sample_door.png"
DEFAULT_STOCK_LIGHT = "res://textures/lights/fragments.png"
DEFAULT_STOCK_TERRAIN = [
    "res://textures/terrain/cobblestone.png",
    "res://textures/terrain/grass.png",
    "res://textures/terrain/dirt.png",
    "res://textures/terrain/sand.png",
    "", "", "", ""
]


class DungeondraftMatcher:
    """Matches map plan elements to Dungeondraft assets in assets.db."""

    def __init__(self, db_path: Optional[Path] = None):
        self.db = AssetDatabase(db_path=db_path)
        self._cache_assets()

    def close(self):
        self.db.close()

    def _cache_assets(self):
        """Pre-fetch categorized asset tables into memory for rapid lookups."""
        cur = self.db.conn.cursor()

        # Walls
        cur.execute("SELECT id, pack_id, res_path, file_name, subpath FROM assets WHERE category = 'walls' AND state = 'ok';")
        self.walls = [dict(row) for row in cur.fetchall()]

        # Portals (doors, windows, arches)
        cur.execute("SELECT id, pack_id, res_path, file_name, subpath FROM assets WHERE category = 'portals' AND state = 'ok';")
        self.portals = [dict(row) for row in cur.fetchall()]

        # Tilesets (floor tiles)
        cur.execute("SELECT id, pack_id, res_path, file_name, subpath FROM assets WHERE category = 'tilesets' AND state = 'ok';")
        self.tilesets = [dict(row) for row in cur.fetchall()]

        # Terrain
        cur.execute("SELECT id, pack_id, res_path, file_name, subpath FROM assets WHERE category = 'terrain' AND state = 'ok';")
        self.terrain = [dict(row) for row in cur.fetchall()]

        # Lights
        cur.execute("SELECT id, pack_id, res_path, file_name, subpath FROM assets WHERE category = 'lights' AND state = 'ok';")
        self.lights = [dict(row) for row in cur.fetchall()]

    def match_wall_texture(self, style_id: str = "", enclosure: str = "masonry") -> Tuple[str, str]:
        """Select a wall texture (res_path, pack_id)."""
        if not self.walls:
            return DEFAULT_STOCK_WALL, "default"

        enclosure = (enclosure or "masonry").lower()
        candidates = []
        for w in self.walls:
            fn = w["file_name"].lower()
            sp = (w["subpath"] or "").lower()
            if enclosure == "timber" or "wood" in style_id:
                if "wood" in fn or "timber" in fn or "wood" in sp:
                    candidates.append(w)
            elif enclosure == "rock" or "cave" in style_id:
                if "rock" in fn or "cave" in fn or "stone" in fn:
                    candidates.append(w)
            else:
                if "wall" in fn or "stone" in fn or "brick" in fn or "masonry" in fn:
                    candidates.append(w)

        if not candidates:
            candidates = self.walls

        # Deterministic pick based on style_id
        idx = int(hashlib.md5(style_id.encode("utf-8")).hexdigest(), 16) % len(candidates)
        pick = candidates[idx]
        return pick["res_path"], pick["pack_id"]

    def match_portal_texture(self, kind: str = "door") -> Tuple[str, str]:
        """Select a door or window portal texture (res_path, pack_id)."""
        if not self.portals:
            return DEFAULT_STOCK_DOOR, "default"

        kind = kind.lower()
        candidates = []
        for p in self.portals:
            fn = p["file_name"].lower()
            if "window" in kind:
                if "window" in fn:
                    candidates.append(p)
            elif "arch" in kind or "opening" in kind:
                if "arch" in fn or "opening" in fn:
                    candidates.append(p)
            else:
                if "door" in fn or "gate" in fn:
                    candidates.append(p)

        if not candidates:
            candidates = self.portals

        pick = candidates[0]
        return pick["res_path"], pick["pack_id"]

    def match_floor_tileset(self, style_id: str = "", room_label: str = "") -> Tuple[str, str]:
        """Select a floor tileset texture (res_path, pack_id)."""
        if not self.tilesets:
            return "", "default"

        style_low = (style_id + " " + room_label).lower()
        candidates = []
        for t in self.tilesets:
            fn = t["file_name"].lower()
            sp = (t["subpath"] or "").lower()
            if any(w in style_low for w in ("wood", "timber", "tavern", "inn", "room")):
                if any(w in fn or w in sp for w in ("wood", "plank", "timber", "floor")):
                    candidates.append(t)
            elif any(w in style_low for w in ("marble", "palace", "temple")):
                if any(w in fn or w in sp for w in ("marble", "tile", "ornate")):
                    candidates.append(t)
            else:
                if any(w in fn or w in sp for w in ("stone", "flagstone", "cobble", "pavement")):
                    candidates.append(t)

        if not candidates:
            candidates = self.tilesets

        pick = candidates[0]
        return pick["res_path"], pick["pack_id"]

    def match_terrain_textures(self, style_id: str = "") -> List[Tuple[str, str]]:
        """Return up to 8 terrain texture slots as [(res_path, pack_id), ...]."""
        slots = []
        if not self.terrain:
            for p in DEFAULT_STOCK_TERRAIN:
                slots.append((p, "default" if p else ""))
            return slots

        style_low = style_id.lower()
        # Find matching textures for primary slots
        cobble = [t for t in self.terrain if "cobble" in t["file_name"].lower() or "stone" in t["file_name"].lower()]
        grass = [t for t in self.terrain if "grass" in t["file_name"].lower()]
        dirt = [t for t in self.terrain if "dirt" in t["file_name"].lower() or "earth" in t["file_name"].lower()]
        sand = [t for t in self.terrain if "sand" in t["file_name"].lower() or "mud" in t["file_name"].lower()]

        t1 = (cobble[0]["res_path"], cobble[0]["pack_id"]) if cobble else (self.terrain[0]["res_path"], self.terrain[0]["pack_id"])
        t2 = (grass[0]["res_path"], grass[0]["pack_id"]) if grass else ("", "")
        t3 = (dirt[0]["res_path"], dirt[0]["pack_id"]) if dirt else ("", "")
        t4 = (sand[0]["res_path"], sand[0]["pack_id"]) if sand else ("", "")

        slots = [t1, t2, t3, t4, ("", ""), ("", ""), ("", ""), ("", "")]
        return slots

    def match_prop(
        self,
        prop_kind: str,
        style_id: str = "",
        room_label: str = "",
        seed: int = 0,
    ) -> Optional[Dict[str, Any]]:
        """Find the best matching object asset in assets.db."""
        clean_kind = re.sub(r"[\d_]+", " ", prop_kind).strip().lower()
        clean_kind = re.sub(r"\s+", " ", clean_kind)

        cur = self.db.conn.cursor()

        # Step 1: Query enrichment for exact object_kind match
        query = """
        SELECT a.id, a.pack_id, a.res_path, a.file_name, a.width, a.height,
               a.grid_w, a.grid_h, a.content_hash, e.object_kind, e.description,
               e.confidence, e.footprint
        FROM assets a
        JOIN enrichment e ON a.content_hash = e.content_hash
        WHERE a.category = 'objects' AND a.state = 'ok'
          AND e.footprint = 'floor'
          AND (e.object_kind = ? OR e.object_kind LIKE ?)
        """
        cur.execute(query, (clean_kind, f"%{clean_kind}%"))
        rows = [dict(r) for r in cur.fetchall()]
        how = "described"

        # Step 2: Fallback query on file_name and subpath in assets table
        if not rows:
            q_terms = clean_kind.split()
            primary_term = q_terms[0] if q_terms else clean_kind
            fallback_query = """
            SELECT a.id, a.pack_id, a.res_path, a.file_name, a.width, a.height,
                   a.grid_w, a.grid_h, a.content_hash, a.file_name as object_kind,
                   '' as description, 0.6 as confidence, 'floor' as footprint
            FROM assets a
            WHERE a.category = 'objects' AND a.state = 'ok'
              AND (a.file_name LIKE ? OR a.subpath LIKE ?)
            LIMIT 50
            """
            cur.execute(fallback_query, (f"%{primary_term}%", f"%{primary_term}%"))
            rows = [dict(r) for r in cur.fetchall()]
            how = "named"

        # There used to be a third query here that returned the first fifty
        # objects in the table when the first two found nothing, so a barrel the
        # library does not have came back as whatever happened to be at the top.
        # That is the quiet failure this whole project is trying to avoid, and
        # it also made the scene check report nothing missing, ever. Say no
        # instead: a prop with no asset is what the prop foundry is for.
        if not rows:
            return None

        # Deterministic selection using seed + prop_kind + room_label
        h = hashlib.sha256(f"{seed}:{prop_kind}:{room_label}".encode("utf-8")).hexdigest()
        pick = dict(rows[int(h, 16) % len(rows)])
        pick["match_quality"] = how
        return pick
