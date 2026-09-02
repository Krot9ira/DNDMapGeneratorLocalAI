#!/usr/bin/env python3
"""Godot 3 PCK and Dungeondraft custom asset pack reader.

Reads standard Godot 3 .pck files and Dungeondraft .dungeondraft_pack files,
extracting manifests (pack.json, default.dungeondraft_tags) and textures
(WebP, PNG, JPG, and StreamTexture .stex within Dungeondraft.pck).
"""
import io
import json
import os
import re
import struct
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from PIL import Image


class PckError(RuntimeError):
    """Raised when a PCK file is corrupted or cannot be read."""
    pass


class PckEntry:
    """Entry in a Godot 3 PCK file table."""
    __slots__ = ("path", "offset", "size", "md5")

    def __init__(self, path: str, offset: int, size: int, md5: bytes):
        self.path = path
        self.offset = offset
        self.size = size
        self.md5 = md5

    def __repr__(self) -> str:
        return f"<PckEntry {self.path} (offset={self.offset}, size={self.size})>"


class PckReader:
    """Reader for Godot 3 .pck and .dungeondraft_pack archives."""

    def __init__(self, file_path: str):
        self.file_path = os.path.abspath(file_path)
        self.entries: Dict[str, PckEntry] = {}
        self.pack_json: Optional[dict] = None
        self.tags_data: Optional[dict] = None
        self.pack_id: Optional[str] = None
        self.is_builtin = False
        self._read_header_and_table()

    def _read_header_and_table(self):
        if not os.path.exists(self.file_path):
            raise PckError(f"Pack file does not exist: {self.file_path}")

        try:
            with open(self.file_path, "rb") as f:
                magic = f.read(4)
                if magic != b"GDPC":
                    raise PckError(f"Invalid magic {magic!r} in {self.file_path} (expected GDPC)")

                fmt, vmaj, vmin, vrev = struct.unpack("<4I", f.read(16))
                f.read(16 * 4)  # 16 reserved uint32s
                (count,) = struct.unpack("<I", f.read(4))

                for _ in range(count):
                    (plen,) = struct.unpack("<I", f.read(4))
                    raw_path = f.read(plen)
                    path = raw_path.rstrip(b"\x00").decode("utf-8", errors="replace")
                    offset, size = struct.unpack("<QQ", f.read(16))
                    md5 = f.read(16)
                    self.entries[path] = PckEntry(path, offset, size, md5)
        except (IOError, struct.error) as exc:
            raise PckError(f"Failed to read PCK table from {self.file_path}: {exc}") from exc

        # Check if this is the built-in Dungeondraft.pck
        if os.path.basename(self.file_path).lower() == "dungeondraft.pck":
            self.is_builtin = True
            self.pack_id = "default"
            self.pack_json = {
                "name": "Dungeondraft Standard Assets",
                "id": "default",
                "version": "1.0",
                "author": "Megasploot",
                "allow_3rd_party_mapping_software_to_read": True,
            }
        else:
            self._load_pack_metadata()

    def _load_pack_metadata(self):
        # Look for pack.json (e.g. res://packs/<id>/pack.json)
        pack_json_entries = [p for p in self.entries if p.endswith("pack.json")]
        if pack_json_entries:
            try:
                raw = self.read_bytes(pack_json_entries[0])
                self.pack_json = json.loads(raw.decode("utf-8", errors="replace"))
                self.pack_id = self.pack_json.get("id")
            except Exception:
                self.pack_json = None

        # Look for default.dungeondraft_tags
        tag_entries = [p for p in self.entries if p.endswith(".dungeondraft_tags")]
        if tag_entries:
            try:
                raw = self.read_bytes(tag_entries[0])
                self.tags_data = json.loads(raw.decode("utf-8", errors="replace"))
            except Exception:
                self.tags_data = None

    def read_bytes(self, res_path_or_entry) -> bytes:
        """Read the exact byte payload of a file within the pack."""
        if isinstance(res_path_or_entry, PckEntry):
            entry = res_path_or_entry
        else:
            entry = self.entries.get(res_path_or_entry)
            if not entry:
                raise KeyError(f"Entry {res_path_or_entry} not found in {self.file_path}")

        with open(self.file_path, "rb") as f:
            f.seek(entry.offset)
            return f.read(entry.size)

    def extract_image(self, res_path: str) -> Optional[Image.Image]:
        """Extract and decode an image asset into a PIL Image.

        Handles standard WebP/PNG/JPG textures as well as Godot .import stex
        references inside Dungeondraft.pck.
        """
        # Case 1: Built-in assets inside Dungeondraft.pck mapped via .import
        if self.is_builtin:
            import_path = res_path + ".import"
            if import_path in self.entries:
                try:
                    import_content = self.read_bytes(import_path).decode("utf-8", errors="replace")
                    match = re.search(r'path="([^"]+)"', import_content)
                    if match:
                        stex_res = match.group(1)
                        if stex_res in self.entries:
                            stex_data = self.read_bytes(stex_res)
                            # Godot StreamTexture has a GDST header followed by WebP starting at RIFF
                            riff_idx = stex_data.find(b"RIFF")
                            if riff_idx >= 0:
                                return Image.open(io.BytesIO(stex_data[riff_idx:]))
                except Exception:
                    return None

        # Case 2: Direct file in entries (custom packs or raw images)
        if res_path in self.entries:
            try:
                data = self.read_bytes(res_path)
                return Image.open(io.BytesIO(data))
            except Exception:
                return None

        return None

    def is_allowed_for_third_party(self) -> bool:
        """Returns False if pack.json explicitly sets allow_3rd_party_mapping_software_to_read: false."""
        if self.is_builtin:
            return True
        if self.pack_json:
            return bool(self.pack_json.get("allow_3rd_party_mapping_software_to_read", True))
        return True

    def list_textures(self) -> List[Tuple[str, str, str, str]]:
        """List all texture assets in this pack.

        Returns a list of tuples: (res_path, category, subpath, file_name).
        Categories include: objects, terrain, walls, portals, paths, materials,
        tilesets, patterns, lights, roofs, caves.
        """
        results = []
        # Pattern for textures:
        # Custom pack: res://packs/<id>/textures/<category>/<subpath>/<file_name>
        # Builtin pack: res://textures/<category>/<subpath>/<file_name> (with .import)

        valid_categories = {
            "objects", "terrain", "walls", "portals", "paths",
            "materials", "tilesets", "patterns", "lights", "roofs", "caves"
        }

        if self.is_builtin:
            for path in self.entries:
                if not path.endswith(".import"):
                    continue
                # res://textures/<category>/.../filename.png.import
                base_path = path[:-7]  # strip '.import'
                if not base_path.startswith("res://textures/"):
                    continue
                parts = base_path[len("res://textures/"):].split("/")
                if not parts:
                    continue
                category = parts[0]
                if category not in valid_categories:
                    continue
                file_name = parts[-1]
                subpath = "/".join(parts[1:-1]) if len(parts) > 2 else ""
                results.append((base_path, category, subpath, file_name))
        else:
            for path in self.entries:
                ext = Path(path).suffix.lower()
                if ext not in (".png", ".webp", ".jpg", ".jpeg"):
                    continue
                if path.endswith("preview.png") or path.endswith("preview.webp"):
                    continue

                # res://packs/<id>/textures/<category>/... or res://textures/<category>/...
                if "/textures/" not in path:
                    continue
                tex_idx = path.find("/textures/") + len("/textures/")
                rel = path[tex_idx:]
                parts = rel.split("/")
                if not parts:
                    continue
                category = parts[0]
                if category not in valid_categories:
                    continue
                file_name = parts[-1]
                subpath = "/".join(parts[1:-1]) if len(parts) > 2 else ""
                results.append((path, category, subpath, file_name))

        return results


class PckWriter:
    """Creates Godot 3 .pck and Dungeondraft .dungeondraft_pack archives."""

    def __init__(self, output_path: str):
        self.output_path = os.path.abspath(output_path)
        self.files: List[Tuple[str, bytes]] = []

    def add_file(self, res_path: str, data: bytes):
        """Add a file to the archive with its Godot res:// path."""
        if not res_path.startswith("res://"):
            res_path = "res://" + res_path.lstrip("/")
        self.files.append((res_path, data))

    def add_from_disk(self, res_path: str, disk_path: str):
        """Add a file from disk to the archive."""
        with open(disk_path, "rb") as f:
            self.add_file(res_path, f.read())

    def write(self):
        """Write the packed .pck file to disk."""
        out_dir = os.path.dirname(self.output_path)
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)

        count = len(self.files)
        # Header size: 4 (GDPC) + 16 (ver) + 64 (reserved) + 4 (count) = 88 bytes
        header_size = 88
        table_size = 0
        file_entries = []
        for res_path, data in self.files:
            path_bytes = res_path.encode("utf-8")
            entry_size = 4 + len(path_bytes) + 8 + 8 + 16
            table_size += entry_size
            file_entries.append((path_bytes, len(data)))

        current_offset = header_size + table_size

        with open(self.output_path, "wb") as f:
            # Magic & version
            f.write(b"GDPC")
            f.write(struct.pack("<4I", 1, 3, 4, 2))  # format=1, vmaj=3, vmin=4, vrev=2
            f.write(b"\x00" * (16 * 4))               # 16 reserved uint32s
            f.write(struct.pack("<I", count))

            # File table
            for (path_bytes, size), (res_path, data) in zip(file_entries, self.files):
                f.write(struct.pack("<I", len(path_bytes)))
                f.write(path_bytes)
                f.write(struct.pack("<QQ", current_offset, size))
                f.write(b"\x00" * 16)  # MD5 (zeros)
                current_offset += size

            # Data payload
            for res_path, data in self.files:
                f.write(data)

