#!/usr/bin/env python3
"""Verify, checksum, and atomically publish an immutable matched image set."""
import argparse
import os
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.release.manifest import digest, verify_manifest

SLOT_FILES = ("SPL_APP_BIN", "UBOOT_ITB", "BASE_DTB", "RADIO_IMAGE", "KERNEL_IMAGE", "ROOTFS_IMAGE")


def assets():
    values = dict(line.split("=", 1) for line in (ROOT / "configs/esp32s31-layout.cfg").read_text().splitlines()
                  if line and not line.startswith("#") and "=" in line)
    names = [values[key] for key in SLOT_FILES] + [values["OUT_IMAGE"], "radio.json", "build-manifest.json"]
    if any(Path(name).name != name for name in names):
        raise ValueError("release asset must be a plain filename")
    return names


def checksums(directory):
    content = "".join(digest(directory / name) + "  " + name + "\n" for name in assets())
    temporary = directory / ".SHA256SUMS.tmp"
    temporary.write_text(content)
    temporary.replace(directory / "SHA256SUMS")


def publish(images, destination, output_root):
    verify_manifest(images / "build-manifest.json", output_root)
    identity = digest(images / "build-manifest.json")[:16]
    destination.mkdir(parents=True, exist_ok=True)
    target = destination / identity
    temporary = Path(tempfile.mkdtemp(prefix=".publish-", dir=destination))
    try:
        for name in assets():
            shutil.copy2(images / name, temporary / name)
        verify_manifest(temporary / "build-manifest.json", output_root, check_inputs=False)
        checksums(temporary)
        if target.exists():
            verify_manifest(target / "build-manifest.json", output_root, check_inputs=False)
            if (target / "SHA256SUMS").read_bytes() != (temporary / "SHA256SUMS").read_bytes():
                raise ValueError("existing immutable publication differs")
        else:
            temporary.rename(target)
        # Readers see either the old complete directory or the new complete one.
        link = destination / f".current-{os.getpid()}"
        try:
            link.symlink_to(target.name, target_is_directory=True)
            link.replace(destination / "current")
        finally:
            link.unlink(missing_ok=True)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path)
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--checksums", action="store_true")
    parser.add_argument("--publish", type=Path)
    args = parser.parse_args()
    root = (args.output_root or ROOT / "out").resolve()
    images = args.prefix or root / "images"
    try:
        if args.checksums:
            verify_manifest(images / "build-manifest.json", root)
            checksums(images)
        if args.publish:
            print(publish(images, args.publish, root))
        elif not args.checksums:
            for name in assets() + ["SHA256SUMS"]:
                print(images / name)
    except (ValueError, OSError, KeyError) as error:
        parser.exit(1, "assets: " + str(error) + "\n")


if __name__ == "__main__":
    main()
