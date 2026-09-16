#!/usr/bin/env python3
"""Validate gallery manifest consistency.

- Runs build-gallery.py --check (expects exit 0).
- Parses manifest.json and verifies schema, slug format, file existence,
  id uniqueness, and that no unregistered images exist in the gallery dir.
"""
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = ROOT / "scripts" / "build-gallery.py"
MANIFEST_PATH = ROOT / "site" / "assets" / "gallery" / "manifest.json"
GALLERY_DIR = ROOT / "site" / "assets" / "gallery"
VALID_SLUG = set("abcdefghijklmnopqrstuvwxyz0123456789-")
IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png"}


class GalleryManifestTest(unittest.TestCase):
    def test_build_check_passes(self):
        result = subprocess.run(
            [sys.executable, str(BUILD_SCRIPT), "--check"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"--check failed:\n{result.stderr}")

    def test_manifest_schema(self):
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        self.assertIn("version", manifest)
        self.assertEqual(manifest["version"], 1)
        self.assertIn("images", manifest)
        self.assertIsInstance(manifest["images"], list)

    def test_image_entries_valid(self):
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        ids_seen = set()
        for entry in manifest["images"]:
            # Required fields
            for key in ("id", "file", "title", "category"):
                self.assertIn(key, entry, f"missing '{key}' in entry {entry}")

            slug = entry["id"]
            # Slug format
            self.assertTrue(
                all(c in VALID_SLUG for c in slug),
                f"invalid slug characters in '{slug}'",
            )
            # Uniqueness
            self.assertNotIn(slug, ids_seen, f"duplicate id '{slug}'")
            ids_seen.add(slug)

            # File matches id
            self.assertEqual(
                Path(entry["file"]).stem,
                slug,
                f"file stem does not match id for '{slug}'",
            )

            # File exists
            file_path = GALLERY_DIR / entry["file"]
            self.assertTrue(file_path.exists(), f"missing file {entry['file']}")

    def test_no_unregistered_images(self):
        """Every image in the gallery dir must appear in the manifest."""
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        manifest_files = {entry["file"] for entry in manifest["images"]}
        for entry in GALLERY_DIR.iterdir():
            if entry.name == "manifest.json":
                continue
            if entry.suffix.lower() in IMAGE_EXTENSIONS:
                self.assertIn(
                    entry.name,
                    manifest_files,
                    f"image {entry.name} exists in gallery but is not in manifest",
                )


if __name__ == "__main__":
    unittest.main(verbosity=2)