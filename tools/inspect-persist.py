#!/usr/bin/env python3
"""Inspect the Chiffon persistence journal in a raw disk image."""

import argparse
import os
from pathlib import Path
import stat
import struct
import sys
import zlib


SECTOR = 512
SLOT_SECTORS = {"standard": 4096, "tiny": 128}
HEADER_VERSIONS = {b"CHIFLOG1": 1, b"CHIFLOG2": 2}
RECORD_MAGIC = 0xC1F0C0DE
RECORD_HEADER = struct.Struct("<6I")
RECORD_NAMES = {1: "PUT", 2: "DEL", 3: "RENAME", 4: "CHMOD"}
PATH_MAX = 256


def read_at(image, offset, count):
    image.seek(offset)
    data = image.read(count)
    if len(data) != count:
        raise ValueError(f"short read at image byte {offset}")
    return data


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def ext_identity(image, sectors, base):
    if base + 4 > sectors:
        return None
    sb = read_at(image, (base + 2) * SECTOR, 1024)
    if sb[56:58] != b"\x53\xef":
        return None
    block_shift = u32(sb, 24)
    if block_shift > 6:
        raise ValueError(f"invalid ext block size at LBA {base}")
    blocks = u32(sb, 4)
    if u32(sb, 76) and u32(sb, 96) & 0x80:
        blocks |= u32(sb, 0x150) << 32
    end = base + blocks * (2 << block_shift)
    if not blocks or end > sectors:
        raise ValueError(f"ext filesystem at LBA {base} extends beyond the image")

    fsid = 0xCBF29CE484222325
    for byte in sb[104:120]:
        fsid = ((fsid ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return end, fsid


def find_ext(image, sectors, partition):
    extents = {}
    mbr = read_at(image, 0, SECTOR)
    if mbr[510:] == b"\x55\xaa":
        for number in range(1, 5):
            entry = 446 + (number - 1) * 16
            kind = mbr[entry + 4]
            base = u32(mbr, entry + 8)
            size = u32(mbr, entry + 12)
            if not (kind or base or size):
                continue
            end = base + size
            if not kind or not base or not size or end > sectors or kind in (0x05, 0x0F, 0x85, 0xEE):
                raise ValueError(f"invalid or unsupported MBR partition {number}")
            if any(base < other_end and other_base < end for other_base, other_end in extents.values()):
                raise ValueError(f"overlapping MBR partition {number}")
            extents[number] = (base, end)
    if not extents and not partition:
        identity = ext_identity(image, sectors, 0)
        if identity:
            return 0, *identity, identity[0]
    occupied_end = max((end for _, end in extents.values()), default=0)
    for number, (base, end) in extents.items():
        if not partition or partition == number:
            identity = ext_identity(image, end, base)
            if identity:
                return base, *identity, occupied_end
    raise ValueError("could not identify an ext root filesystem")


def read_slot_header(data):
    version = HEADER_VERSIONS.get(data[:8])
    if version is None:
        status = "empty or invalidated" if not any(data) else "unrecognized header"
        return None, status
    generation, fsid, stored_crc = struct.unpack_from("<QQ8xI", data, 8)
    details = f"CHIFLOG{version}, generation {generation}, filesystem ID {fsid:016x}"
    if zlib.crc32(data[:32]) != stored_crc:
        return None, f"{details}, header CRC mismatch"
    return (version, generation, fsid), f"{details}, header CRC valid"


def record_path(path):
    return (2 <= len(path) < PATH_MAX and path.startswith(b"/")
            and b"\0" not in path
            and path not in (b"/proc", b"/dev")
            and not path.startswith((b"/proc/", b"/dev/")))


def inspect_records(data, version, records_lba, show_records):
    pos = 0
    count = 0
    view = memoryview(data)
    while pos + RECORD_HEADER.size <= len(data):
        magic, kind, mode, plen, dlen, stored_crc = RECORD_HEADER.unpack_from(view, pos)
        if not magic:
            reason = "zero end marker"
            break
        if magic != RECORD_MAGIC:
            reason = "invalid record magic"
            break
        body = pos + RECORD_HEADER.size
        end = body + plen + dlen
        if end > len(data):
            reason = "record extends beyond the slot"
            break
        crc = zlib.crc32(view[body:end], zlib.crc32(view[pos:pos + 20]))
        if crc != stored_crc:
            reason = "record CRC mismatch"
            break
        path = bytes(view[body:body + plen])
        if (kind not in RECORD_NAMES or not record_path(path)
                or (kind == 3 and not record_path(bytes(view[body + plen:end])))
                or (kind in (2, 4) and dlen)):
            reason = "record failed kernel metadata or path validation (persistence disabled)"
            break

        if show_records:
            payload = f"{dlen} data bytes"
            if kind == 3:
                payload = f"destination {bytes(view[body + plen:end])!r}"
            print(f"  +{pos:7d} {RECORD_NAMES[kind]:6s} {path!r}, mode {mode:#o}, {payload}")
        pos = ((end + SECTOR - 1) // SECTOR) * SECTOR if version == 2 else end
        count += 1
    else:
        reason = "slot exhausted" if pos == len(data) else "fewer than 24 bytes remain"

    stop_byte = records_lba * SECTOR + pos
    print(f"  Valid record prefix: {count} records, {pos} bytes")
    print(f"  Stop at image byte {stop_byte} (LBA {stop_byte // SECTOR}): {reason}")
    report_tail(data[pos:])


def report_tail(data):
    nonzero = len(data) - data.count(b"\0")
    print(f"  Ignored bytes: {len(data)}, including {nonzero} nonzero bytes")
    if nonzero:
        print("  Ignored data can include stale bytes or unpublished writes.")


def inspect_image(image, layout, partition, show_records):
    info = os.fstat(image.fileno())
    if not stat.S_ISREG(info.st_mode):
        raise ValueError("expected a regular raw disk image file")
    sectors, remainder = divmod(info.st_size, SECTOR)
    if not sectors or remainder:
        raise ValueError("image size must be a nonzero multiple of 512 bytes")
    slot_sectors = SLOT_SECTORS[layout]
    region = sectors - 2 * slot_sectors
    if region <= 0:
        raise ValueError(f"image is too small for the {layout} journal layout")
    base, ext_end, fsid, occupied_end = find_ext(image, sectors, partition)
    if occupied_end > region:
        raise ValueError(f"filesystem or partition overlaps the {layout} journal area")

    print(f"Image: {sectors} sectors, {layout} journal at LBA {region}")
    print(f"Ext root: LBA {base}, ends before LBA {ext_end}, filesystem ID {fsid:016x}")
    headers = []
    active = None
    for slot in range(2):
        lba = region + slot * slot_sectors
        header, status = read_slot_header(read_at(image, lba * SECTOR, SECTOR))
        headers.append((lba, header, status))
        if header and header[2] == fsid:
            if active is None or header[1] > headers[active][1][1]:
                active = slot

    if active is None:
        print("Kernel slot selection: no valid header matches this filesystem")
    else:
        print(f"Kernel slot selection: slot {active}, generation {headers[active][1][1]}")
    for slot, (lba, header, status) in enumerate(headers):
        print(f"Slot {slot} at LBA {lba}: {status}")
        if header and header[2] != fsid:
            print("  Filesystem ID does not match the ext root.")
        data = read_at(image, (lba + 1) * SECTOR, (slot_sectors - 1) * SECTOR)
        if header:
            inspect_records(data, header[0], lba + 1, show_records)
        else:
            report_tail(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="raw disk image from a stopped guest")
    parser.add_argument("--layout", choices=SLOT_SECTORS, default="standard",
                        help="journal layout used by the guest kernel (default: standard)")
    parser.add_argument("--partition", type=int, choices=range(1, 5), default=0,
                        help="MBR root partition when explicitly selected at boot")
    parser.add_argument("--records", action="store_true",
                        help="show record metadata and escaped paths")
    args = parser.parse_args()
    try:
        if not args.image.is_file():
            raise ValueError("expected a regular raw disk image file")
        with args.image.open("rb") as image:
            inspect_image(image, args.layout, args.partition, args.records)
    except (OSError, ValueError) as error:
        print(f"{parser.prog}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
