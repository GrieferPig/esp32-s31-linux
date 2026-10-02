#!/usr/bin/env python3
"""Generate checksums for image artifacts used in release validation."""
import argparse
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOT_FILES = ("SPL_APP_BIN", "UBOOT_ITB", "BASE_DTB", "RADIO_IMAGE", "KERNEL_IMAGE", "ROOTFS_IMAGE")

def assets():
    values = dict(line.split("=", 1) for line in (ROOT / "configs/esp32s31-layout.cfg").read_text().splitlines() if line and not line.startswith("#") and "=" in line)
    return [values[key] for key in SLOT_FILES] + [values["OUT_IMAGE"], "build-manifest.json"]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, default=Path("build"))
    parser.add_argument("--checksums", action="store_true")
    args = parser.parse_args()
    if args.checksums:
        lines = []
        for name in assets():
            with (args.prefix / name).open("rb") as stream:
                lines.append(hashlib.file_digest(stream, "sha256").hexdigest() + "  " + name)
        (args.prefix / "SHA256SUMS").write_text("\n".join(lines) + "\n")
    else:
        for name in assets() + ["SHA256SUMS"]:
            print(args.prefix / name)

if __name__ == "__main__":
    main()
