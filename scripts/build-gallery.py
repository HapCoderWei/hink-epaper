#!/usr/bin/env python3
"""Scan site/assets/gallery/ and generate or verify manifest.json.

Usage:
    python3 scripts/build-gallery.py          # generate manifest
    python3 scripts/build-gallery.py --check   # verify only (exit non-zero on failure)
"""
import json
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GALLERY_DIR = ROOT / "site" / "assets" / "gallery"
MANIFEST_PATH = GALLERY_DIR / "manifest.json"

MAX_SIZE = 300 * 1024  # 300 KiB
MAX_IMAGES = 24
ASPECT_TARGET = 250 / 122
ASPECT_TOLERANCE = 0.01
VALID_SLUG = set("abcdefghijklmnopqrstuvwxyz0123456789-")
IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png"}

# ── Title mapping table ──────────────────────────────────────────────
# New images must be registered here before they can be included.
TITLE_MAP = {
    "coca-cola-logo": {"title": "Coca-Cola · 红色版", "category": "品牌标志"},
    "force-logo-red": {"title": "Force Logo · 红色版", "category": "标语"},
    "pepsi-logo": {"title": "Pepsi · 红色版", "category": "品牌标志"},
}


def read_png_dimensions(data: bytes):
    """Return (width, height) from a PNG IHDR chunk."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    w = struct.unpack(">I", data[16:20])[0]
    h = struct.unpack(">I", data[20:24])[0]
    return w, h


def read_jpeg_dimensions(data: bytes):
    """Return (width, height) from a JPEG by scanning SOF0/SOF2 markers."""
    if data[:2] != b"\xff\xd8":
        return None
    i = 2
    while i < len(data) - 1:
        if data[i] != 0xFF:
            break
        marker = data[i + 1]
        if marker == 0xD9 or marker == 0xDA:  # EOI or SOS
            break
        length = struct.unpack(">H", data[i + 2 : i + 4])[0]
        if marker in (0xC0, 0xC2):  # SOF0 or SOF2
            h = struct.unpack(">H", data[i + 5 : i + 7])[0]
            w = struct.unpack(">H", data[i + 7 : i + 9])[0]
            return w, h
        i += 2 + length
    return None


def read_image_dimensions(path: Path):
    """Read image dimensions from header without external dependencies."""
    data = path.read_bytes()
    result = read_png_dimensions(data)
    if result:
        return result
    result = read_jpeg_dimensions(data)
    if result:
        return result
    return None


def validate_aspect(width: int, height: int) -> bool:
    """Check |w/h - 250/122| / (250/122) <= 0.01."""
    actual = width / height
    return abs(actual - ASPECT_TARGET) / ASPECT_TARGET <= ASPECT_TOLERANCE


def collect_images():
    """Collect and validate all images in the gallery directory."""
    images = []
    for entry in sorted(GALLERY_DIR.iterdir()):
        if entry.name == "manifest.json":
            continue
        if entry.suffix.lower() not in IMAGE_EXTENSIONS:
            continue
        images.append(entry)
    return images


def validate(images):
    """Validate all images; return list of (id, file, title, category) or exit on error."""
    if len(images) > MAX_IMAGES:
        print(f"Error: {len(images)} images found, maximum is {MAX_IMAGES}.", file=sys.stderr)
        sys.exit(1)

    entries = []
    seen_ids = set()

    for path in images:
        slug = path.stem
        # Slug format
        if not all(c in VALID_SLUG for c in slug):
            print(f"Error: invalid slug '{slug}' in {path.name}; only [a-z0-9-] allowed.", file=sys.stderr)
            sys.exit(1)
        if slug in seen_ids:
            print(f"Error: duplicate id '{slug}' in {path.name}.", file=sys.stderr)
            sys.exit(1)
        seen_ids.add(slug)

        # Size
        size = path.stat().st_size
        if size > MAX_SIZE:
            print(f"Error: {path.name} is {size} bytes, exceeds {MAX_SIZE} byte limit.", file=sys.stderr)
            sys.exit(1)

        # Aspect ratio
        dims = read_image_dimensions(path)
        if dims is None:
            print(f"Error: cannot read dimensions of {path.name}.", file=sys.stderr)
            sys.exit(1)
        w, h = dims
        if not validate_aspect(w, h):
            ratio = w / h
            print(
                f"Error: {path.name} has aspect ratio {w}×{h} ({ratio:.4f}), "
                f"expected ~{ASPECT_TARGET:.4f} (250:122 ±1%).",
                file=sys.stderr,
            )
            sys.exit(1)

        # Title mapping
        if slug not in TITLE_MAP:
            print(f"Error: slug '{slug}' not found in TITLE_MAP; register it before including.", file=sys.stderr)
            sys.exit(1)

        info = TITLE_MAP[slug]
        entries.append({
            "id": slug,
            "file": path.name,
            "title": info["title"],
            "category": info["category"],
        })

    return entries


def build_manifest(entries):
    return {"version": 1, "images": entries}


def main():
    check_mode = "--check" in sys.argv

    images = collect_images()
    entries = validate(images)
    manifest = build_manifest(entries)

    if check_mode:
        # In check mode, also verify the on-disk manifest matches
        if MANIFEST_PATH.exists():
            on_disk = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
            if on_disk != manifest:
                print("Error: on-disk manifest.json does not match expected content.", file=sys.stderr)
                sys.exit(1)
        else:
            print("Error: manifest.json does not exist.", file=sys.stderr)
            sys.exit(1)
        print(f"Check passed: {len(entries)} image(s), manifest consistent.")
    else:
        content = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
        MANIFEST_PATH.write_text(content, encoding="utf-8")
        print(f"Wrote {MANIFEST_PATH} with {len(entries)} image(s).")


if __name__ == "__main__":
    main()
