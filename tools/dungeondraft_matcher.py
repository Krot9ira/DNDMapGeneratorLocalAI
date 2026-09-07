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
from typing import Any, Dict, List, Optional, Set, Tuple

from dungeondraft_db import AssetDatabase, DB_PATH_DEFAULT

# Default fallbacks if database is minimal
DEFAULT_STOCK_WALL = "res://textures/walls/stone.png"
DEFAULT_STOCK_DOOR = "res://textures/portals/door_00.png"
DEFAULT_STOCK_LIGHT = "res://textures/lights/fragments.png"
DEFAULT_STOCK_TERRAIN = [
    "res://textures/terrain/terrain_limestone.png",
    "res://textures/terrain/terrain_grass.png",
    "res://textures/terrain/terrain_dirt.png",
    "res://textures/terrain/terrain_sand.png",
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
        # "<wall>_end" is the cap sprite Dungeondraft draws at the tip of a run,
        # not a wall texture. Picking one paints the whole wall as a thin dark
        # line with no masonry in it, so it is never a candidate.
        usable = [w for w in self.walls
                  if not w["file_name"].lower().rsplit(".", 1)[0].endswith("_end")]
        if not usable:
            return DEFAULT_STOCK_WALL, "default"

        candidates = []
        for w in usable:
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
            candidates = usable

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

    # What the ground of a place is made of, keyed on words in its style id.
    # The first list that matches wins, so "forest_swamp" reads as swamp.
    GROUND_BY_STYLE = [
        (("swamp", "marsh", "bog", "fen", "mire"), ("swamp", "mud", "moss")),
        (("forest", "glade", "grove", "wood", "jungle", "meadow"), ("grass", "moss")),
        (("snow", "ice", "frozen", "winter", "tundra"), ("snow", "ice")),
        (("desert", "dune", "oasis", "wasteland"), ("sand", "sandstone")),
        (("cave", "cavern", "underdark", "mine", "gorge"), ("rocky", "gravel", "stone")),
        (("ruin", "graveyard", "camp", "field", "pass", "mountain"), ("dirt", "earth", "gravel")),
    ]

    def _terrain_like(self, words: Tuple[str, ...],
                      taken: Optional[Set[str]] = None) -> Optional[Tuple[str, str]]:
        """First indexed terrain whose file name mentions one of words.

        Slots already spoken for are skipped: two slots holding the same
        texture is a wasted slot, and nothing the assembler paints into it
        would be visible as a different kind of ground.
        """
        for word in words:
            for t in self.terrain:
                if word in t["file_name"].lower() and t["res_path"] not in (taken or ()):
                    return t["res_path"], t["pack_id"]
        return None

    def match_terrain_textures(self, style_id: str = "") -> List[Tuple[str, str]]:
        """Return 8 terrain texture slots as [(res_path, pack_id), ...].

        The order is fixed and the assembler paints against it: 1 is the ground
        the place is mostly made of, 2 is its greenery, 3 its bare earth, 4 the
        silt under standing water.
        """
        if not self.terrain:
            return [(p, "default" if p else "") for p in DEFAULT_STOCK_TERRAIN]

        style_low = (style_id or "").lower()
        ground_words = ("cobble", "stone", "flagstone", "pavement")
        for keys, words in self.GROUND_BY_STYLE:
            if any(k in style_low for k in keys):
                ground_words = words
                break

        wet = any(k in style_low for k in ("swamp", "marsh", "bog", "cave", "cavern", "sewer"))
        blank = ("", "")
        slots = []
        taken: Set[str] = set()
        for words in (ground_words,
                      ("moss", "grass") if wet else ("grass", "moss"),
                      ("dirt", "earth", "gravel"),
                      ("mud", "sand", "gravel") if wet else ("sand", "gravel", "mud")):
            pick = self._terrain_like(words, taken)
            if pick is None and not slots:
                pick = (self.terrain[0]["res_path"], self.terrain[0]["pack_id"])
            slots.append(pick or blank)
            if pick:
                taken.add(pick[0])

        return slots + [blank] * 4

    # Words that describe a thing rather than name it. "fallen birch log" is a
    # log; matching on "fallen" is what turned it into a door, and matching
    # "moss covered granite slab" on "moss" turned it into a tree trunk. A
    # candidate that only ever agrees about one of these has not matched.
    MODIFIERS = {
        "abandoned", "ancient", "big", "black", "blue", "broken", "burnt", "burned",
        "charred", "collapsed", "covered", "cracked", "crumbling", "damp", "dark",
        "dead", "dense", "dirty", "dry", "empty", "fallen", "flickering", "full",
        "giant", "glowing", "gnarled", "green", "grey", "gray", "half", "huge",
        "large", "little", "long", "loose", "low", "moldering", "mossy", "muddy",
        "narrow", "new", "old", "open", "overgrown", "pale", "piled", "red",
        "rotten", "rotting", "ruined", "rusted", "rusty", "scattered", "shallow",
        "short", "small", "smashed", "spilled", "stacked", "tall", "tangled",
        "thick", "thin", "tiny", "torn", "twisted", "weathered", "wet", "white",
        "wide", "worn", "and", "the", "with",
        # What a thing is made of describes it too, and so does the surface it
        # hangs on: "charred wooden tripod" is a tripod, not a wooden fence,
        # and "dense bramble wall" is brambles, not a wall torch.
        "brass", "canvas", "clay", "cloth", "copper", "iron", "leather",
        "metal", "steel", "wicker", "wood", "wooden",
        "wall", "floor", "ground",
    }

    # Words the library keeps under another name. Added to the search, and
    # worth half of what the plan's own word is worth, so "thick fern cluster"
    # still lands on a fern rather than on the mushroom cluster that "cluster"
    # drags in.
    SYNONYMS = {
        "rock": ("boulder", "stone"),
        "boulder": ("rock", "stone"),
        "pebble": ("gravel", "rock"),
        "thicket": ("shrub", "bush", "fern"),
        "bramble": ("shrub", "bush", "briar"),
        "litter": ("leaf", "leaves"),
        "leaf": ("leaves",),
        "cluster": ("mushroom", "fungus"),
        "fungal": ("mushroom", "fungus"),
        "spore": ("mushroom", "fungus"),
        "sapling": ("tree", "shrub"),
        "knot": ("root",),
        "tangle": ("root", "vine"),
        "drape": ("vine", "ivy"),
        "ivy": ("vine", "creeper"),
        "rut": ("track", "path"),
        "patch": ("dirt", "mud", "grass"),
        "remain": ("bone", "skeleton", "debris"),
        "fragment": ("bone", "debris", "rubble"),
        "mote": ("spark", "firefly", "light"),
        "pit": ("firepit", "campfire", "fire"),
        "tripod": ("campfire", "cooking", "pot"),
        "pot": ("pottery", "urn", "jug"),
        "frame": ("tent",),
        "tunnel": ("log", "hollow"),
        "undergrowth": ("fern", "shrub", "bush", "grass"),
    }

    # Words the plan uses that no file is named after, where searching for the
    # word itself only ever finds something else wearing it as an adjective:
    # every "stone" in the library is a stone pillar or a stone statue. The
    # replacement is searched instead of the word, not alongside it.
    REPLACEMENTS = {
        "stone": ("rock", "boulder"),
        "slab": ("rock", "boulder"),
        "granite": ("rock", "boulder"),
        "wisp": ("candle", "lantern", "light"),
    }

    # Where a thing cannot plausibly come from. An open-air site borrowing from
    # the furniture drawer is how a forest ends up with an Egyptian table in
    # it, and from the farming drawer is how tree roots become vegetables; a
    # room borrowing from the tree drawer is the same mistake indoors. A
    # penalty rather than a filter, so a camp that really does want a table
    # still gets one - the request only has to say so.
    # Kept deliberately narrow. Penalising "furniture" wholesale outdoors was
    # worse than the problem: a camp has chairs and tables in it, and the
    # penalty only pushed the honest furniture/chairs below a bath chair filed
    # under activities. What is listed here is what belongs indoors or in a
    # trade and nowhere else.
    OUT_OF_PLACE = {
        "open": ("doors", "kitchen", "bathing", "beds", "books", "shops",
                 "instruments", "farming", "cooking", "alchemy", "lumberyard",
                 "dungeon"),
        "masonry": ("vegetation", "trees", "shrubs", "ferns", "roots"),
        "timber": ("vegetation", "trees", "shrubs", "ferns", "roots"),
        "rock": ("doors", "kitchen", "bathing", "beds", "books", "shops",
                 "farming", "lumberyard"),
    }

    # Families that are only ever right when the request names them. The stock
    # library files every plain rock under clutter/lava_rocks, so "rock" in a
    # forest came back as a lump of volcanic glass; a scene that actually wants
    # one says "lava" and keeps it.
    ONLY_IF_ASKED = ("lava", "sewer", "gore", "blood")

    _TOKEN_SPLIT = re.compile(r"[^a-z0-9]+")

    @staticmethod
    def _singular(word: str) -> str:
        """Fold a plural onto its singular so "pebbles" finds Pebble_01.

        Crude on purpose: the library is named in English and the cases that
        matter are regular. Anything shorter than five letters is left alone,
        because "grass" and "moss" are not plurals.
        """
        if len(word) > 4:
            if word.endswith("ies"):
                return word[:-3] + "y"
            if word.endswith("ses") or word.endswith("hes") or word.endswith("xes"):
                return word[:-2]
            if word.endswith("s") and not word.endswith("ss"):
                return word[:-1]
        return word

    @classmethod
    def _tokens(cls, text: str) -> List[str]:
        """Meaningful singular lowercase words in a name. Digits are noise."""
        out = []
        for word in cls._TOKEN_SPLIT.split((text or "").lower()):
            if len(word) > 2 and not word.isdigit():
                out.append(cls._singular(word))
        return out

    def _score_candidate(self, row: Dict[str, Any], weights: Dict[str, float],
                         content: Set[str], enclosure: str) -> float:
        """How well one asset answers the request. Zero or less means "no".

        Two things decide it. Whether the file agrees about a word that names
        something - a candidate matching only "fallen" or "old" has not matched
        at all. And how much of the file's own name the request explains: for
        "stone", pillar_stone_30 is half about something else and
        statue_gargoyle_stone_04 is two thirds about something else, which is
        the difference between a standing stone and a gargoyle.
        """
        stem = str(row.get("file_name") or "").rsplit(".", 1)[0]
        file_tokens = self._tokens(stem)
        if not file_tokens:
            return -1.0
        path_tokens = set(self._tokens(row.get("subpath") or ""))

        hits_file = {t: weights[t] for t in file_tokens if t in weights}
        hits_path = {t: weights[t] for t in path_tokens if t in weights}
        if not (set(hits_file) & content) and not (set(hits_path) & content):
            return -1.0

        # How much of the file's name the request accounts for, counted by
        # words rather than by how confident we are about each: boulder_08 is
        # entirely a boulder, while rock_lava_09 is half about lava, which the
        # request never mentioned. Weighing this by confidence let a lava rock
        # beat a boulder for "rock", because "rock" was the asked-for word and
        # "boulder" only a guess at it - but the lava is wrong either way.
        coverage = len(hits_file) / float(len(file_tokens))
        recall = (sum(hits_file.values()) + sum(hits_path.values())) / float(sum(weights.values()))
        score = 3.0 * coverage + 2.0 * min(1.0, recall)
        # The library names a thing after itself first: boulder_12, fern_03,
        # cauldron_04. Agreeing about that first word is the strongest signal
        # there is, and disagreeing about it is how a statue answers for a stone.
        score += 2.0 * weights.get(file_tokens[0], 0.0)
        if hits_path:
            score += 0.5
        for bad in self.OUT_OF_PLACE.get(enclosure, ()) + self.ONLY_IF_ASKED:
            if (bad in path_tokens or bad in file_tokens) and bad not in weights:
                score -= 2.5
                break
        return score

    def match_prop(
        self,
        prop_kind: str,
        style_id: str = "",
        room_label: str = "",
        seed: int = 0,
        enclosure: str = "",
    ) -> Optional[Dict[str, Any]]:
        """Find the best matching object asset in assets.db."""
        clean_kind = re.sub(r"[\d_]+", " ", prop_kind).strip().lower()
        clean_kind = re.sub(r"\s+", " ", clean_kind)

        cur = self.db.conn.cursor()

        # Step 1: Query enrichment for exact object_kind match
        query = """
        SELECT a.id, a.pack_id, a.res_path, a.file_name, a.subpath, a.width,
               a.height, a.grid_w, a.grid_h, a.content_hash, e.object_kind,
               e.description, e.confidence, e.footprint
        FROM assets a
        JOIN enrichment e ON a.content_hash = e.content_hash
        WHERE a.category = 'objects' AND a.state = 'ok'
          AND e.footprint = 'floor'
          AND (e.object_kind = ? OR e.object_kind LIKE ?)
        """
        cur.execute(query, (clean_kind, f"%{clean_kind}%"))
        rows = [dict(r) for r in cur.fetchall()]
        how = "described"

        # Step 2: Fall back to what the files are called. Every word of the
        # request is asked about, not just the first one - "dense fern
        # undergrowth" is a fern, and asking only about "dense" is what left a
        # library holding sixty-four ferns reporting that it had nothing.
        if not rows:
            request = set(self._tokens(clean_kind))
            if not request:
                return None

            # What the plan actually said is worth twice what we guessed it
            # might have meant, so a guess never outvotes the request.
            weights: Dict[str, float] = {}
            content: Set[str] = set()
            for word in request:
                replacement = self.REPLACEMENTS.get(word)
                if replacement:
                    for alt in replacement:
                        weights[alt] = max(weights.get(alt, 0.0), 1.0)
                        content.add(alt)
                    continue
                weights[word] = 1.0
                if word not in self.MODIFIERS:
                    content.add(word)
                    # A guess about what the word means still names something -
                    # "twisted dead sapling" is a tree - so it counts as an
                    # answer, just a quieter one than the plan's own word.
                    for alt in self.SYNONYMS.get(word, ()):
                        weights.setdefault(alt, 0.5)
                        content.add(alt)
            if not content:
                # Nothing but adjectives. Let them name the thing rather than
                # refusing outright.
                content = set(weights)
            if not weights:
                return None

            seen: Dict[str, Dict[str, Any]] = {}
            fallback_query = """
            SELECT a.id, a.pack_id, a.res_path, a.file_name, a.subpath,
                   a.width, a.height, a.grid_w, a.grid_h, a.content_hash,
                   a.file_name as object_kind, '' as description,
                   0.6 as confidence, 'floor' as footprint
            FROM assets a
            WHERE a.category = 'objects' AND a.state = 'ok'
              AND (a.file_name LIKE ? OR a.subpath LIKE ?)
            LIMIT 400
            """
            for term in sorted(weights, key=len, reverse=True):
                cur.execute(fallback_query, (f"%{term}%", f"%{term}%"))
                for r in cur.fetchall():
                    row = dict(r)
                    seen.setdefault(row["res_path"], row)

            scored = []
            for row in seen.values():
                score = self._score_candidate(row, weights, content, enclosure)
                if score > 0:
                    scored.append((score, row))
            if scored:
                best = max(s for s, _ in scored)
                # Everything within a hair of the best answers equally well, so
                # the seed picks among them and one asset does not carpet a map.
                rows = [r for s, r in scored if s >= best - 0.25]
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
        rows.sort(key=lambda r: r["res_path"])
        h = hashlib.sha256(f"{seed}:{prop_kind}:{room_label}".encode("utf-8")).hexdigest()
        pick = dict(rows[int(h, 16) % len(rows)])
        pick["match_quality"] = how
        return pick
