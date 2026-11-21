#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import hashlib
import io
from pathlib import Path
import re
import tarfile


def unpack(archive, digest, output):
    limits = {"spl-p4-200.bin": 0xB040, "uboot-p4.bin": 0x2B000}
    if not re.fullmatch("[0-9a-f]{64}", digest):
        raise ValueError("expected the release asset's SHA-256")
    if archive.stat().st_size > 256 * 1024:
        raise ValueError("P4 boot archive exceeds 256 KiB")
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError("P4 boot archive SHA-256 mismatch")
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as tar:
        members = tar.getmembers()
        if len(members) != len(limits) or {entry.name for entry in members} != limits.keys():
            raise ValueError("expected exactly spl-p4-200.bin and uboot-p4.bin")
        if any(not entry.isfile() or not 0 < entry.size <= limits[entry.name] for entry in members):
            raise ValueError("invalid P4 boot component type or size")
        payloads = {entry.name: tar.extractfile(entry).read() for entry in members}
    output.mkdir()
    for name, contents in payloads.items():
        (output / name).write_bytes(contents)


def main():
    parser = argparse.ArgumentParser(description="Verify and unpack the ESP32-P4 boot release.")
    parser.add_argument("archive", type=Path)
    parser.add_argument("sha256")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        unpack(args.archive, args.sha256, args.output)
    except (OSError, ValueError, tarfile.TarError) as error:
        parser.exit(1, f"P4 boot firmware: {error}\n")


if __name__ == "__main__":
    main()
