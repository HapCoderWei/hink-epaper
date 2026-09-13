#!/usr/bin/env python3
"""OTA v2 hardware checks over BLE, safe by default.

All default and staging actions avoid installation. The only path that sends
ARM_INSTALL/INSTALL requires both an explicit action and --allow-install.
"""

import argparse
import asyncio
from pathlib import Path
import struct
import zlib

from bleak import BleakClient, BleakScanner


OTA_CHARACTERISTIC = "0000331f-0000-1000-8000-00805f9b34fb"
EPD_CHARACTERISTIC = "4b646063-6264-f3a7-8941-e65356ea82fe"

PROTOCOL = 1
BOARD_ID = 0x213A
CMD_INFO = 0x10
CMD_BEGIN = 0x11
CMD_ERASE_NEXT = 0x12
CMD_DATA = 0x13
CMD_FINISH = 0x14
CMD_STATUS = 0x15
CMD_ABORT = 0x16
CMD_ARM_INSTALL = 0x17
CMD_INSTALL = 0x18
ARM_CONFIRM = 0x314D5241
INSTALL_CONFIRM = 0x31534E49
STATUS_OK = 0
STATUS_UNKNOWN_COMMAND = 9
PHASE_IDLE = 0
PHASE_ERASING = 1
PHASE_RECEIVING = 2
PHASE_VERIFIED = 3
PHASE_ARMED = 5
PHASE_INSTALL_PENDING = 6


def deterministic_image():
    """Return a two-sector non-installable payload for destructive staging tests."""
    image = bytearray(((index * 73 + 19) & 0xFF) for index in range(8192))
    # M1-B requires the source byte at the Telink boot-marker offset to be K,
    # while keeping the actual inactive-slot byte erased as 0xff.
    image[8] = 0x4B
    return bytes(image)


def parse_status(data, expected_command):
    if len(data) < 20 or data[0:2] != bytes((0xA2, PROTOCOL)):
        raise RuntimeError(f"invalid OTA response: {data.hex()}")
    if data[2] != expected_command:
        raise RuntimeError(
            f"response order mismatch: expected 0x{expected_command:02x}, "
            f"received 0x{data[2]:02x}"
        )
    received, image_size, crc32 = struct.unpack_from("<III", data, 8)
    status = {
        "command": data[2],
        "status": data[3],
        "phase": data[4],
        "erased": data[5],
        "sectors": data[6],
        "max_data": data[7],
        "received": received,
        "image_size": image_size,
        "crc32": crc32,
    }
    if len(data) >= 24:
        status.update(
            capabilities=data[20],
            current_slot=data[21],
            target_slot=data[22],
            milestone=data[23],
        )
    else:
        status.update(
            capabilities=0,
            current_slot=None,
            target_slot=None,
            milestone=0,
        )
    status["firmware_version"] = (
        struct.unpack_from("<I", data, 24)[0] if len(data) >= 28 else None
    )
    status.update(
        recovery_state=data[28] if len(data) >= 32 else None,
        recovery_action=data[29] if len(data) >= 32 else None,
        journal_valid=data[30] if len(data) >= 32 else None,
        journal_corrupt=data[31] if len(data) >= 32 else None,
    )
    return status


class OtaConnection:
    def __init__(self, device):
        self.device = device
        self.client = None
        self.notifications = asyncio.Queue()

    async def __aenter__(self):
        last_error = None
        for attempt in range(1, 4):
            self.client = BleakClient(self.device, timeout=15.0)
            try:
                await self.client.connect()
                await self.client.start_notify(
                    OTA_CHARACTERISTIC, self._on_notification
                )
                return self
            except Exception as error:
                last_error = error
                if self.client.is_connected:
                    await self.client.disconnect()
                if attempt < 3:
                    # CoreBluetooth can briefly hide a peripheral immediately
                    # after disconnect even though a cached direct connection
                    # already works. Give the radio time to settle, then create
                    # a fresh client/backend for the next attempt.
                    await asyncio.sleep(2.0 * attempt)
        raise RuntimeError("BLE reconnect failed after 3 attempts") from last_error

    async def __aexit__(self, exc_type, exc, traceback):
        if self.client and self.client.is_connected:
            await self.client.disconnect()

    def _on_notification(self, _sender, data):
        self.notifications.put_nowait(bytes(data))

    async def command(self, payload, timeout=5.0, attempts=1):
        for attempt in range(1, attempts + 1):
            await self.client.write_gatt_char(
                OTA_CHARACTERISTIC, bytes(payload), response=True
            )
            try:
                response = await asyncio.wait_for(
                    self.notifications.get(), timeout=timeout
                )
                return parse_status(response, payload[0])
            except asyncio.TimeoutError:
                if attempt == attempts:
                    raise RuntimeError(
                        f"OTA command 0x{payload[0]:02x} notification timeout"
                    )
        raise AssertionError("unreachable")

    async def info(self):
        # CoreBluetooth can lose the first notification while the peripheral's
        # initial connection-parameter update is still in flight. INFO is
        # read-only, so exactly one same-connection retry is safe.
        return await self.command((CMD_INFO,), timeout=3.0, attempts=2)


async def find_tag(name_prefix):
    devices = await BleakScanner.discover(timeout=10.0, return_adv=True)
    matches = []
    for device, advertisement in devices.values():
        name = advertisement.local_name or device.name or ""
        if name.startswith(name_prefix):
            matches.append((advertisement.rssi, name, device))
    if not matches:
        raise RuntimeError(f"no BLE device beginning with {name_prefix!r} found")
    matches.sort(reverse=True, key=lambda item: item[0])
    return matches[0][1], matches[0][2]


async def resolve_tag(name_prefix, address):
    if address:
        # On macOS the CoreBluetooth UUID is stable and can be connected to
        # directly from its cache. Requiring a fresh scan here makes the test
        # flaky because advertisements can disappear briefly after disconnect.
        return name_prefix, address
    return await find_tag(name_prefix)


def begin_packet(image):
    return (
        bytes((CMD_BEGIN, PROTOCOL))
        + struct.pack("<HII", BOARD_ID, len(image), zlib.crc32(image))
    )


def load_manifested_image(filename):
    image = Path(filename).read_bytes()
    if len(image) < 56:
        raise RuntimeError("firmware is too short for a Telink header and HINK manifest")
    payload = image[:-24]
    fields = struct.unpack("<4sBBHIIII", image[-24:])
    magic, format_version, flags, board_id = fields[:4]
    payload_length, payload_crc, firmware_version, manifest_crc = fields[4:]
    if magic != b"HOTA" or format_version != 1 or flags != 0:
        raise RuntimeError("firmware has an invalid HINK manifest header")
    if board_id != BOARD_ID or payload_length != len(payload):
        raise RuntimeError("firmware manifest board or payload length is incompatible")
    if payload_crc != (zlib.crc32(payload) & 0xFFFFFFFF):
        raise RuntimeError("firmware payload CRC32 is invalid")
    if manifest_crc != (zlib.crc32(image[-24:-4]) & 0xFFFFFFFF):
        raise RuntimeError("firmware manifest CRC32 is invalid")
    telink_length = struct.unpack_from("<I", payload, 0x18)[0]
    if payload[8:12] != b"KNLT" or telink_length + 4 != len(payload):
        raise RuntimeError("firmware Telink header or length is invalid")
    telink_crc = struct.unpack(">I", payload[-4:])[0]
    if telink_crc != (zlib.crc32(payload[:-4]) & 0xFFFFFFFF):
        raise RuntimeError("firmware Telink CRC32 is invalid")
    return image, firmware_version


async def send_prefix(connection, image, limit, max_data):
    offset = 0
    while offset < limit:
        page_remaining = 0x100 - (offset & 0xFF)
        length = min(max_data, page_remaining, limit - offset)
        packet = bytes((CMD_DATA,)) + struct.pack("<I", offset) + image[offset:offset + length]
        status = await connection.command(packet)
        if status["status"] != STATUS_OK:
            raise RuntimeError(f"DATA failed at {offset}: {status}")
        offset += length
    return offset


async def run(name_prefix, address):
    name, device = await resolve_tag(name_prefix, address)
    print(f"DEVICE={name}")

    # A deterministic two-sector payload is sufficient to exercise both
    # interruption phases without pretending it is an installable firmware.
    image = deterministic_image()

    async with OtaConnection(device) as connection:
        info = await connection.info()
        if info["status"] != STATUS_OK:
            raise RuntimeError(f"INFO failed: {info}")
        print(
            f"OTA_MILESTONE={info['milestone']} "
            f"CURRENT_SLOT={info['current_slot']} TARGET_SLOT={info['target_slot']} "
            f"INSTALL_CAPABILITY={info['capabilities'] & 1}"
        )
        characteristic_uuids = {
            characteristic.uuid
            for service in connection.client.services
            for characteristic in service.characteristics
        }
        if EPD_CHARACTERISTIC not in characteristic_uuids:
            raise RuntimeError("normal HINK image characteristic is missing")

        legacy = await connection.command((0x07, 0, 0, 0, 0))
        if legacy["status"] != STATUS_UNKNOWN_COMMAND:
            raise RuntimeError(f"legacy install command was not rejected: {legacy}")
        print("PASS legacy ATC command 0x07 rejected")

        started = await connection.command(begin_packet(image))
        if started["status"] != STATUS_OK or started["sectors"] != 2:
            raise RuntimeError(f"BEGIN failed: {started}")
        erased = await connection.command((CMD_ERASE_NEXT,), timeout=15.0)
        if erased["status"] != STATUS_OK or erased["erased"] != 1:
            raise RuntimeError(f"ERASE_NEXT failed: {erased}")
        print("INTERRUPT erase phase after 1/2 sectors")

    await asyncio.sleep(2.0)
    name, device = await resolve_tag(name_prefix, address)
    async with OtaConnection(device) as connection:
        info = await connection.info()
        if info["status"] != STATUS_OK:
            raise RuntimeError(f"reconnect after erase interruption failed: {info}")
        print("PASS reconnect after erase interruption")

        started = await connection.command(begin_packet(image))
        if started["status"] != STATUS_OK:
            raise RuntimeError(f"restart BEGIN failed: {started}")
        for expected in (1, 2):
            erased = await connection.command((CMD_ERASE_NEXT,), timeout=15.0)
            if erased["status"] != STATUS_OK or erased["erased"] != expected:
                raise RuntimeError(f"erase {expected}/2 failed: {erased}")
        sent = await send_prefix(connection, image, 480, started["max_data"])
        print(f"INTERRUPT receive phase after {sent} bytes")

    await asyncio.sleep(2.0)
    name, device = await resolve_tag(name_prefix, address)
    async with OtaConnection(device) as connection:
        info = await connection.info()
        if info["status"] != STATUS_OK:
            raise RuntimeError(f"reconnect after data interruption failed: {info}")
        if info["phase"] != 2 or info["received"] != 480:
            raise RuntimeError(f"unexpected retained receive state: {info}")
        aborted = await connection.command((CMD_ABORT,))
        if aborted["status"] != STATUS_OK or aborted["phase"] != 0:
            raise RuntimeError(f"ABORT failed: {aborted}")
        print("PASS reconnect after receive interruption; session aborted")

    print("PASS OTA v2 M0/M1-B non-installing hardware interruption checks")


async def stage_and_verify(name_prefix, address, firmware):
    """Upload and fully verify a manifested image without installing it."""
    image, firmware_version = load_manifested_image(firmware)
    name, device = await resolve_tag(name_prefix, address)
    print(f"DEVICE={name}")
    print(
        f"CANDIDATE_FIRMWARE_VERSION={firmware_version} "
        f"BYTES={len(image)} CRC32={zlib.crc32(image) & 0xFFFFFFFF:08X}"
    )

    async with OtaConnection(device) as connection:
        info = await connection.info()
        if info["status"] != STATUS_OK:
            raise RuntimeError(f"INFO failed: {info}")
        if info["milestone"] < 1:
            raise RuntimeError(f"device does not support M1 inactive-slot staging: {info}")
        print(
            f"OTA_MILESTONE={info['milestone']} "
            f"CURRENT_SLOT={info['current_slot']} TARGET_SLOT={info['target_slot']} "
            f"INSTALL_CAPABILITY={info['capabilities'] & 1} "
            f"CURRENT_FIRMWARE_VERSION={info['firmware_version']}"
        )

        status = await connection.command(begin_packet(image))
        expected_sectors = (len(image) + 0xFFF) // 0x1000
        if status["status"] != STATUS_OK or status["sectors"] != expected_sectors:
            raise RuntimeError(f"BEGIN failed: {status}")
        while status["erased"] < status["sectors"]:
            status = await connection.command((CMD_ERASE_NEXT,), timeout=15.0)
            if status["status"] != STATUS_OK:
                raise RuntimeError(f"ERASE_NEXT failed: {status}")
        print(f"PASS erased inactive target slot sectors={expected_sectors}")

        sent = await send_prefix(connection, image, len(image), status["max_data"])
        if sent != len(image):
            raise RuntimeError(f"upload stopped early at {sent}/{len(image)}")
        print(f"PASS uploaded and immediately read back {sent} bytes")

        status = await connection.command((CMD_FINISH,), timeout=15.0)
        expected_crc = zlib.crc32(image) & 0xFFFFFFFF
        if (
            status["status"] != STATUS_OK
            or status["phase"] != PHASE_VERIFIED
            or status["received"] != len(image)
            or status["crc32"] != expected_crc
        ):
            raise RuntimeError(f"FINISH did not verify the candidate: {status}")
        print("PASS device full-flash, manifest, Telink header and CRC verification")
        print("PASS candidate remains non-bootable and no install command was sent")


async def install_staged(name_prefix, address, firmware):
    """Explicitly arm and install an already verified candidate."""
    image, candidate_version = load_manifested_image(firmware)
    image_crc = zlib.crc32(image) & 0xFFFFFFFF
    name, device = await resolve_tag(name_prefix, address)
    print(f"DEVICE={name}")

    async with OtaConnection(device) as connection:
        info = await connection.info()
        if not (info["capabilities"] & 1):
            raise RuntimeError(f"device does not advertise M1 install capability: {info}")
        if (
            info["phase"] != PHASE_VERIFIED
            or info["image_size"] != len(image)
            or info["crc32"] != image_crc
        ):
            raise RuntimeError(f"staged candidate does not match the selected file: {info}")
        old_slot = info["current_slot"]
        new_slot = info["target_slot"]
        old_version = info["firmware_version"]
        print(
            f"INSTALL_FROM_SLOT={old_slot} INSTALL_TO_SLOT={new_slot} "
            f"CURRENT_VERSION={old_version} CANDIDATE_VERSION={candidate_version}"
        )

        arm = bytes((CMD_ARM_INSTALL,)) + struct.pack("<II", ARM_CONFIRM, image_crc)
        status = await connection.command(arm, timeout=20.0)
        if status["status"] != STATUS_OK or status["phase"] != PHASE_ARMED:
            raise RuntimeError(f"ARM_INSTALL failed: {status}")
        print("PASS ARM_INSTALL revalidated the candidate")

        install = bytes((CMD_INSTALL,)) + struct.pack(
            "<II", INSTALL_CONFIRM, image_crc
        )
        status = await connection.command(install, timeout=20.0)
        if status["status"] != STATUS_OK or status["phase"] != PHASE_INSTALL_PENDING:
            raise RuntimeError(f"INSTALL was not queued: {status}")
        print("PASS INSTALL queued; waiting for marker switch and reboot")

    await asyncio.sleep(4.0)
    name, device = await resolve_tag(name_prefix, address)
    async with OtaConnection(device) as connection:
        info = await connection.info()
        if info["current_slot"] != new_slot or info["target_slot"] != old_slot:
            raise RuntimeError(f"device did not boot from the candidate slot: {info}")
        if info["firmware_version"] != candidate_version:
            raise RuntimeError(f"device booted an unexpected firmware version: {info}")
        if info["phase"] != PHASE_IDLE:
            raise RuntimeError(f"new firmware did not start with an idle session: {info}")
    print(
        f"PASS rebooted into slot={new_slot} firmware_version={candidate_version}"
    )


async def prepare_install_power_cut(name_prefix, address, firmware, checkpoint):
    """Start an install whose diagnostic source image waits after INSTALL_INTENT."""
    image, candidate_version = load_manifested_image(firmware)
    image_crc = zlib.crc32(image) & 0xFFFFFFFF
    name, device = await resolve_tag(name_prefix, address)
    print(f"DEVICE={name}")

    async with OtaConnection(device) as connection:
        info = await connection.info()
        if not (info["capabilities"] & 1):
            raise RuntimeError(f"device does not advertise M1 install capability: {info}")
        if (
            info["phase"] != PHASE_VERIFIED
            or info["image_size"] != len(image)
            or info["crc32"] != image_crc
        ):
            raise RuntimeError(f"staged candidate does not match the selected file: {info}")
        print(
            f"INSTALL_FROM_SLOT={info['current_slot']} "
            f"INSTALL_TO_SLOT={info['target_slot']} "
            f"CURRENT_VERSION={info['firmware_version']} "
            f"CANDIDATE_VERSION={candidate_version}"
        )

        arm = bytes((CMD_ARM_INSTALL,)) + struct.pack("<II", ARM_CONFIRM, image_crc)
        status = await connection.command(arm, timeout=20.0)
        if status["status"] != STATUS_OK or status["phase"] != PHASE_ARMED:
            raise RuntimeError(f"ARM_INSTALL failed: {status}")
        print("PASS ARM_INSTALL revalidated the candidate")

        install = bytes((CMD_INSTALL,)) + struct.pack(
            "<II", INSTALL_CONFIRM, image_crc
        )
        status = await connection.command(install, timeout=20.0)
        if status["status"] != STATUS_OK or status["phase"] != PHASE_INSTALL_PENDING:
            raise RuntimeError(f"INSTALL was not queued: {status}")

        # The diagnostic source waits forever after the journaled intent. Give
        # its deferred installer enough time to reach that checkpoint before
        # telling the operator it is safe to remove target power.
        await asyncio.sleep(1.0)
        print(f"READY_FOR_POWER_CUT phase={checkpoint}")


async def prepare_power_cut(name_prefix, address, phase):
    """Leave the staging session at a known boundary before target power loss."""
    name, device = await resolve_tag(name_prefix, address)
    image = deterministic_image()
    print(f"DEVICE={name}")

    async with OtaConnection(device) as connection:
        info = await connection.info()
        if info["status"] != STATUS_OK:
            raise RuntimeError(f"INFO failed: {info}")

        started = await connection.command(begin_packet(image))
        if started["status"] != STATUS_OK or started["sectors"] != 2:
            raise RuntimeError(f"BEGIN failed: {started}")

        if phase == "erase":
            status = await connection.command((CMD_ERASE_NEXT,), timeout=15.0)
            if (
                status["status"] != STATUS_OK
                or status["phase"] != PHASE_ERASING
                or status["erased"] != 1
            ):
                raise RuntimeError(f"failed to stage erase interruption: {status}")
            print("READY_FOR_POWER_CUT phase=erase erased=1/2")
            return

        for expected in (1, 2):
            status = await connection.command((CMD_ERASE_NEXT,), timeout=15.0)
            if status["status"] != STATUS_OK or status["erased"] != expected:
                raise RuntimeError(f"erase {expected}/2 failed: {status}")
        sent = await send_prefix(connection, image, 480, started["max_data"])
        status = await connection.command((CMD_STATUS,))
        if status["phase"] != PHASE_RECEIVING or status["received"] != sent:
            raise RuntimeError(f"failed to stage receive interruption: {status}")
        print(f"READY_FOR_POWER_CUT phase=receive received={sent}/8192")


async def check_after_power_cut(name_prefix, address):
    """Require a clean boot and idle OTA session after target power is restored."""
    name, device = await resolve_tag(name_prefix, address)
    print(f"DEVICE={name}")
    async with OtaConnection(device) as connection:
        info = await connection.info()
        if info["status"] != STATUS_OK or info["phase"] != PHASE_IDLE:
            raise RuntimeError(f"OTA session did not reset cleanly: {info}")
        characteristic_uuids = {
            characteristic.uuid
            for service in connection.client.services
            for characteristic in service.characteristics
        }
        if EPD_CHARACTERISTIC not in characteristic_uuids:
            raise RuntimeError("normal HINK image characteristic is missing")
    print("PASS clean boot and idle OTA session after target power cycle")


async def show_info(name_prefix, address):
    """Read and print the current OTA identity without modifying flash."""
    name, device = await resolve_tag(name_prefix, address)
    print(f"DEVICE={name}")
    async with OtaConnection(device) as connection:
        info = await connection.info()
    print(
        f"OTA_MILESTONE={info['milestone']} CURRENT_SLOT={info['current_slot']} "
        f"TARGET_SLOT={info['target_slot']} INSTALL_CAPABILITY={info['capabilities'] & 1} "
        f"RECOVERY_LOG_CAPABILITY={(info['capabilities'] >> 1) & 1} "
        f"FIRMWARE_VERSION={info['firmware_version']} PHASE={info['phase']} "
        f"RECOVERY_STATE={info['recovery_state']} "
        f"RECOVERY_ACTION={info['recovery_action']} "
        f"JOURNAL_VALID={info['journal_valid']} "
        f"JOURNAL_CORRUPT={info['journal_corrupt']}"
    )


async def display_test_pattern(name_prefix, address):
    """Exercise the normal image characteristic using the built-in pattern."""
    name, device = await resolve_tag(name_prefix, address)
    print(f"DEVICE={name}")
    async with OtaConnection(device) as connection:
        await connection.client.write_gatt_char(
            EPD_CHARACTERISTIC, bytes((0x05,)), response=True
        )
    print("PASS built-in three-colour test-pattern command accepted")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--name-prefix", default="HINK_C4363D", help="BLE local-name prefix"
    )
    parser.add_argument(
        "--address",
        help="known CoreBluetooth UUID; avoids repeated macOS discovery between reconnects",
    )
    parser.add_argument(
        "--firmware",
        help="manifested firmware used by staging or explicit install actions",
    )
    parser.add_argument(
        "--action",
        choices=(
            "disconnect-test",
            "prepare-power-erase",
            "prepare-power-receive",
            "check-power",
            "display-test",
            "stage-verify",
            "install-staged",
            "prepare-power-install",
            "info",
        ),
        default="disconnect-test",
        help="hardware check to run; prepare actions stop at a safe chunk boundary",
    )
    parser.add_argument(
        "--allow-install",
        action="store_true",
        help="required safety gate for explicit install actions",
    )
    parser.add_argument(
        "--power-cut-phase",
        choices=("install-intent", "markers-switched"),
        default="install-intent",
        help="diagnostic checkpoint expected from prepare-power-install",
    )
    args = parser.parse_args()
    if args.action == "disconnect-test":
        coroutine = run(args.name_prefix, args.address)
    elif args.action == "prepare-power-erase":
        coroutine = prepare_power_cut(args.name_prefix, args.address, "erase")
    elif args.action == "prepare-power-receive":
        coroutine = prepare_power_cut(args.name_prefix, args.address, "receive")
    elif args.action == "check-power":
        coroutine = check_after_power_cut(args.name_prefix, args.address)
    elif args.action == "info":
        coroutine = show_info(args.name_prefix, args.address)
    elif args.action == "stage-verify":
        if not args.firmware:
            parser.error("--firmware is required for --action stage-verify")
        coroutine = stage_and_verify(args.name_prefix, args.address, args.firmware)
    elif args.action == "install-staged":
        if not args.allow_install:
            parser.error("--allow-install is required for --action install-staged")
        if not args.firmware:
            parser.error("--firmware is required for --action install-staged")
        coroutine = install_staged(args.name_prefix, args.address, args.firmware)
    elif args.action == "prepare-power-install":
        if not args.allow_install:
            parser.error(
                "--allow-install is required for --action prepare-power-install"
            )
        if not args.firmware:
            parser.error("--firmware is required for --action prepare-power-install")
        coroutine = prepare_install_power_cut(
            args.name_prefix, args.address, args.firmware, args.power_cut_phase
        )
    else:
        coroutine = display_test_pattern(args.name_prefix, args.address)
    asyncio.run(coroutine)


if __name__ == "__main__":
    main()
