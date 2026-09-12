#!/usr/bin/env python3
"""Append, inspect, and verify the HINK OTA v2 firmware manifest."""

import argparse
import json
import os
import struct
import sys
import tempfile
import zlib


MANIFEST_MAGIC = b"HOTA"
MANIFEST_FORMAT_VERSION = 1
MANIFEST_SIZE = 24
DEFAULT_BOARD_ID = 0x213A
MANIFEST_STRUCT = struct.Struct("<4sBBHIIII")


class ManifestError(ValueError):
    pass


def parse_integer(value):
    try:
        return int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected an integer, for example 1 or 0x213a") from error


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def native_crc_details(payload):
    if len(payload) < 4:
        return None, None, False
    stored = struct.unpack(">I", payload[-4:])[0]
    calculated = crc32(payload[:-4])
    return stored, calculated, stored == calculated


def decode_manifest(image):
    if len(image) < MANIFEST_SIZE:
        raise ManifestError("file is shorter than the 24-byte HINK manifest")

    values = MANIFEST_STRUCT.unpack(image[-MANIFEST_SIZE:])
    return {
        "magic": values[0],
        "format_version": values[1],
        "flags": values[2],
        "board_id": values[3],
        "payload_length": values[4],
        "payload_crc32": values[5],
        "firmware_version": values[6],
        "manifest_crc32": values[7],
    }


def inspect_image(image, expected_board=None):
    errors = []
    try:
        manifest = decode_manifest(image)
    except ManifestError as error:
        return {"valid": False, "errors": [str(error)]}

    payload = image[:-MANIFEST_SIZE]
    calculated_payload_crc = crc32(payload)
    calculated_manifest_crc = crc32(image[-MANIFEST_SIZE:-4])
    native_stored, native_calculated, native_valid = native_crc_details(payload)

    if manifest["magic"] != MANIFEST_MAGIC:
        errors.append("manifest magic is not HOTA")
    if manifest["format_version"] != MANIFEST_FORMAT_VERSION:
        errors.append("unsupported manifest format version")
    if manifest["flags"] != 0:
        errors.append("manifest flags must be zero for M1")
    if expected_board is not None and manifest["board_id"] != expected_board:
        errors.append(
            "board mismatch: manifest is 0x%04x, expected 0x%04x"
            % (manifest["board_id"], expected_board)
        )
    if manifest["payload_length"] != len(payload):
        errors.append(
            "payload length mismatch: manifest is %d, file contains %d"
            % (manifest["payload_length"], len(payload))
        )
    if manifest["payload_crc32"] != calculated_payload_crc:
        errors.append(
            "payload CRC32 mismatch: manifest is %08X, calculated %08X"
            % (manifest["payload_crc32"], calculated_payload_crc)
        )
    if manifest["manifest_crc32"] != calculated_manifest_crc:
        errors.append(
            "manifest CRC32 mismatch: manifest is %08X, calculated %08X"
            % (manifest["manifest_crc32"], calculated_manifest_crc)
        )
    if not native_valid:
        if native_stored is None:
            errors.append("payload is too short to contain the Telink CRC32")
        else:
            errors.append(
                "Telink CRC32 mismatch: stored %08X, calculated %08X"
                % (native_stored, native_calculated)
            )

    result = {
        "valid": not errors,
        "errors": errors,
        "file_length": len(image),
        "manifest_size": MANIFEST_SIZE,
        "magic": manifest["magic"].decode("ascii", errors="replace"),
        "format_version": manifest["format_version"],
        "flags": manifest["flags"],
        "board_id": manifest["board_id"],
        "payload_length": manifest["payload_length"],
        "payload_crc32": manifest["payload_crc32"],
        "calculated_payload_crc32": calculated_payload_crc,
        "firmware_version": manifest["firmware_version"],
        "manifest_crc32": manifest["manifest_crc32"],
        "calculated_manifest_crc32": calculated_manifest_crc,
        "telink_crc32": native_stored,
        "calculated_telink_crc32": native_calculated,
        "telink_crc32_valid": native_valid,
    }
    return result


def build_manifest(payload, board_id, firmware_version):
    if not 0 <= board_id <= 0xFFFF:
        raise ManifestError("board ID does not fit in 16 bits")
    if not 0 <= firmware_version <= 0xFFFFFFFF:
        raise ManifestError("firmware version does not fit in 32 bits")
    if len(payload) > 0xFFFFFFFF:
        raise ManifestError("payload length does not fit in 32 bits")

    native_stored, native_calculated, native_valid = native_crc_details(payload)
    if not native_valid:
        if native_stored is None:
            raise ManifestError("payload is too short to contain the Telink CRC32")
        raise ManifestError(
            "Telink CRC32 is missing or invalid: stored %08X, calculated %08X"
            % (native_stored, native_calculated)
        )

    prefix = struct.pack(
        "<4sBBHIII",
        MANIFEST_MAGIC,
        MANIFEST_FORMAT_VERSION,
        0,
        board_id,
        len(payload),
        crc32(payload),
        firmware_version,
    )
    return prefix + struct.pack("<I", crc32(prefix))


def write_atomically(path, data, mode=None):
    directory = os.path.dirname(os.path.abspath(path))
    temporary = tempfile.NamedTemporaryFile(prefix=".ota-manifest-", dir=directory, delete=False)
    temporary_path = temporary.name
    try:
        with temporary:
            temporary.write(data)
            temporary.flush()
            os.fsync(temporary.fileno())
        if mode is not None:
            os.chmod(temporary_path, mode)
        os.replace(temporary_path, path)
    except Exception:
        try:
            os.unlink(temporary_path)
        except FileNotFoundError:
            pass
        raise


def append_file(source_path, output_path, board_id, firmware_version):
    with open(source_path, "rb") as source:
        payload = source.read()

    if len(payload) >= MANIFEST_SIZE and payload[-MANIFEST_SIZE:-MANIFEST_SIZE + 4] == MANIFEST_MAGIC:
        raise ManifestError("file already ends with a HINK manifest")

    manifest = build_manifest(payload, board_id, firmware_version)
    destination = output_path or source_path
    source_mode = os.stat(source_path).st_mode
    write_atomically(destination, payload + manifest, source_mode)

    result = inspect_image(payload + manifest, board_id)
    if not result["valid"]:
        raise ManifestError("new manifest failed verification: " + "; ".join(result["errors"]))
    return result


def load_and_inspect(path, expected_board):
    with open(path, "rb") as image_file:
        return inspect_image(image_file.read(), expected_board)


def printable_result(path, result):
    lines = [
        "%s: %s" % (path, "VALID" if result["valid"] else "INVALID"),
        "  format=%s flags=%s board=0x%04X firmware_version=%s"
        % (
            result.get("format_version", "?"),
            result.get("flags", "?"),
            result.get("board_id", 0),
            result.get("firmware_version", "?"),
        ),
        "  payload_length=%s payload_crc32=%08X manifest_crc32=%08X"
        % (
            result.get("payload_length", "?"),
            result.get("payload_crc32", 0),
            result.get("manifest_crc32", 0),
        ),
        "  telink_crc32=%08X"
        % (result.get("telink_crc32") if result.get("telink_crc32") is not None else 0),
    ]
    for error in result["errors"]:
        lines.append("  ERROR: " + error)
    return "\n".join(lines)


def command_append(args):
    result = append_file(args.image, args.output, args.board, args.version)
    print(printable_result(args.output or args.image, result))
    return 0


def command_inspect(args):
    result = load_and_inspect(args.image, args.board)
    if args.json:
        print(json.dumps(result, sort_keys=True, indent=2))
    else:
        print(printable_result(args.image, result))
    return 0


def command_verify(args):
    result = load_and_inspect(args.image, args.board)
    stream = sys.stdout if result["valid"] else sys.stderr
    print(printable_result(args.image, result), file=stream)
    return 0 if result["valid"] else 1


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    append_parser = subparsers.add_parser("append", help="append a manifest to a Telink binary")
    append_parser.add_argument("image")
    append_parser.add_argument("--output", help="write a new file instead of updating IMAGE")
    append_parser.add_argument("--board", type=parse_integer, default=DEFAULT_BOARD_ID)
    append_parser.add_argument("--version", type=parse_integer, required=True)
    append_parser.set_defaults(function=command_append)

    inspect_parser = subparsers.add_parser("inspect", help="show manifest fields and validation results")
    inspect_parser.add_argument("image")
    inspect_parser.add_argument("--board", type=parse_integer, default=DEFAULT_BOARD_ID)
    inspect_parser.add_argument("--json", action="store_true")
    inspect_parser.set_defaults(function=command_inspect)

    verify_parser = subparsers.add_parser("verify", help="verify a complete manifested image")
    verify_parser.add_argument("image")
    verify_parser.add_argument("--board", type=parse_integer, default=DEFAULT_BOARD_ID)
    verify_parser.set_defaults(function=command_verify)
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.function(args)
    except (ManifestError, OSError) as error:
        print("ERROR: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
