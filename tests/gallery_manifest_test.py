#!/usr/bin/env python3
"""Validate gallery manifest consistency.

- Runs build-gallery.py --check (expects exit 0).
- Parses manifest.json and verifies schema, slug format, file existence,
  id uniqueness, and that no unregistered images exist in the gallery dir.
"""
import json
import re
import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = ROOT / "scripts" / "build-gallery.py"
MANIFEST_PATH = ROOT / "site" / "assets" / "gallery" / "manifest.json"
CATALOG_PATH = ROOT / "site" / "assets" / "gallery" / "catalog.json"
GALLERY_DIR = ROOT / "site" / "assets" / "gallery"
VALID_SLUG_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$")
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
                VALID_SLUG_RE.fullmatch(slug),
                f"invalid slug '{slug}'",
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
            if entry.name in ("manifest.json", "catalog.json"):
                continue
            if entry.suffix.lower() in IMAGE_EXTENSIONS:
                self.assertIn(
                    entry.name,
                    manifest_files,
                    f"image {entry.name} exists in gallery but is not in manifest",
                )

    def test_catalog_schema(self):
        """catalog.json must exist and have valid structure."""
        self.assertTrue(CATALOG_PATH.exists(), "catalog.json must exist")
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        self.assertEqual(catalog.get("version"), 1, "catalog version must be 1")
        self.assertIn("images", catalog)
        self.assertIsInstance(catalog["images"], dict, "catalog images must be an object")

    def test_catalog_entries_valid(self):
        """Each catalog entry must have valid slug, title, and category."""
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        for slug, info in catalog["images"].items():
            self.assertTrue(
                VALID_SLUG_RE.fullmatch(slug),
                f"invalid slug '{slug}' in catalog.json",
            )
            self.assertIn("title", info, f"catalog entry '{slug}' missing title")
            self.assertIn("category", info, f"catalog entry '{slug}' missing category")
            self.assertIsInstance(info["title"], str)
            self.assertIsInstance(info["category"], str)
            self.assertTrue(info["title"].strip())
            self.assertTrue(info["category"].strip())

    def test_catalog_manifest_consistency(self):
        """catalog.json and manifest.json must reference the same set of images."""
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        catalog_slugs = set(catalog["images"].keys())
        manifest_slugs = {entry["id"] for entry in manifest["images"]}
        self.assertEqual(catalog_slugs, manifest_slugs,
            "catalog.json and manifest.json must reference the same image slugs")
        # Verify titles and categories match
        for entry in manifest["images"]:
            slug = entry["id"]
            self.assertEqual(entry["title"], catalog["images"][slug]["title"],
                f"title mismatch for '{slug}'")
            self.assertEqual(entry["category"], catalog["images"][slug]["category"],
                f"category mismatch for '{slug}'")

    def test_browser_python_sort_order(self):
        """Browser (JS) and Python must produce the same slug sort order."""
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        slugs = sorted(catalog["images"].keys())
        # Python's sorted() uses lexicographic order which matches JS .sort()
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        manifest_slugs = [entry["id"] for entry in manifest["images"]]
        self.assertEqual(slugs, manifest_slugs,
            "manifest image order must match sorted catalog slugs")


if __name__ == "__main__":
    unittest.main(verbosity=2)
