#!/usr/bin/env python3
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[1]
TOOL_PATH = ROOT / "firmware" / "tools" / "ota_manifest.py"
SPEC = importlib.util.spec_from_file_location("ota_manifest", TOOL_PATH)
ota_manifest = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ota_manifest)


def make_telink_payload(length=96):
    body = bytearray((index * 29 + 7) & 0xFF for index in range(length - 4))
    body[8:12] = b"KNLT"
    struct.pack_into("<I", body, 0x18, length - 4)
    return bytes(body) + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


class OtaManifestTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory(prefix="hink-ota-manifest-test.")
        self.image_path = Path(self.temp_dir.name) / "firmware.bin"
        self.payload = make_telink_payload()
        self.image_path.write_bytes(self.payload)

    def tearDown(self):
        self.temp_dir.cleanup()

    def run_tool(self, *arguments):
        return subprocess.run(
            [sys.executable, os.fspath(TOOL_PATH), *map(str, arguments)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def append_valid_image(self):
        result = self.run_tool("append", self.image_path, "--board", "0x213a", "--version", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.image_path.read_bytes()), len(self.payload) + 24)

    def rewrite_manifest(self, **changes):
        image = self.image_path.read_bytes()
        manifest = ota_manifest.decode_manifest(image)
        manifest.update(changes)
        prefix = struct.pack(
            "<4sBBHIII",
            manifest["magic"],
            manifest["format_version"],
            manifest["flags"],
            manifest["board_id"],
            manifest["payload_length"],
            manifest["payload_crc32"],
            manifest["firmware_version"],
        )
        self.image_path.write_bytes(image[:-24] + prefix + struct.pack("<I", ota_manifest.crc32(prefix)))

    def test_correct_file_appends_inspects_and_verifies(self):
        self.append_valid_image()
        verify = self.run_tool("verify", self.image_path, "--board", "0x213a")
        self.assertEqual(verify.returncode, 0, verify.stderr)

        inspect = self.run_tool("inspect", self.image_path, "--json")
        self.assertEqual(inspect.returncode, 0, inspect.stderr)
        fields = json.loads(inspect.stdout)
        self.assertTrue(fields["valid"])
        self.assertEqual(fields["magic"], "HOTA")
        self.assertEqual(fields["format_version"], 1)
        self.assertEqual(fields["flags"], 0)
        self.assertEqual(fields["board_id"], 0x213A)
        self.assertEqual(fields["payload_length"], len(self.payload))
        self.assertEqual(fields["firmware_version"], 1)
        self.assertTrue(fields["telink_crc32_valid"])

    def test_wrong_board_is_rejected(self):
        self.append_valid_image()
        self.rewrite_manifest(board_id=0x9999)
        verify = self.run_tool("verify", self.image_path)
        self.assertNotEqual(verify.returncode, 0)
        self.assertIn("board mismatch", verify.stderr)

    def test_wrong_payload_length_is_rejected(self):
        self.append_valid_image()
        self.rewrite_manifest(payload_length=len(self.payload) + 1)
        verify = self.run_tool("verify", self.image_path)
        self.assertNotEqual(verify.returncode, 0)
        self.assertIn("payload length mismatch", verify.stderr)

    def test_tampered_payload_is_rejected(self):
        self.append_valid_image()
        image = bytearray(self.image_path.read_bytes())
        image[40] ^= 0x80
        self.image_path.write_bytes(image)
        verify = self.run_tool("verify", self.image_path)
        self.assertNotEqual(verify.returncode, 0)
        self.assertIn("payload CRC32 mismatch", verify.stderr)
        self.assertIn("Telink CRC32 mismatch", verify.stderr)

    def test_tampered_manifest_is_rejected(self):
        self.append_valid_image()
        image = bytearray(self.image_path.read_bytes())
        image[-8] ^= 0x01
        self.image_path.write_bytes(image)
        verify = self.run_tool("verify", self.image_path)
        self.assertNotEqual(verify.returncode, 0)
        self.assertIn("manifest CRC32 mismatch", verify.stderr)

    def test_append_rejects_missing_telink_crc(self):
        self.image_path.write_bytes(self.payload[:-4])
        append = self.run_tool("append", self.image_path, "--version", "1")
        self.assertNotEqual(append.returncode, 0)
        self.assertIn("Telink CRC32 is missing or invalid", append.stderr)
        self.assertEqual(self.image_path.read_bytes(), self.payload[:-4])

    def test_append_rejects_a_second_manifest(self):
        self.append_valid_image()
        original = self.image_path.read_bytes()
        append = self.run_tool("append", self.image_path, "--version", "2")
        self.assertNotEqual(append.returncode, 0)
        self.assertIn("already ends with a HINK manifest", append.stderr)
        self.assertEqual(self.image_path.read_bytes(), original)


if __name__ == "__main__":
    unittest.main(verbosity=2)
