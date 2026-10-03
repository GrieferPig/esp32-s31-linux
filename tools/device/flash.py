#!/usr/bin/env python3
"""Flash a verified matched image set without building or overwriting persist.

Partial updates fail closed: host artifacts do not prove which companions are
installed on a connected board. Use all slots until device receipts exist.
"""
import argparse
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.checks.layout import flash_layout
from tools.release.manifest import verify_manifest

SLOTS = (("SPL", "spl_app.bin"), ("UBOOT_ITB", "u-boot.itb"),
         ("DTB", "esp32s31_generic.dtb"), ("RADIO", "radio.bin"),
         ("KERNEL", "xipImage"), ("ROOTFS", "rootfs.sqfs"))


def command(output_root, port, baud, esptool="esptool", *, artifact_dir=None):
    # Resolve the publication link once; a later publication cannot switch this run.
    images = (artifact_dir if artifact_dir is not None else output_root / "images").resolve()
    slots, capacities = flash_layout()
    for slot, name in SLOTS:
        size = (images / name).stat().st_size
        if not 0 < size <= capacities[slot]:
            raise ValueError(f"{slot} image size {size} exceeds capacity {capacities[slot]} or is empty")
    verify_manifest(images / "build-manifest.json", output_root,
                    check_inputs=artifact_dir is None)
    argv = [esptool, "--chip", "esp32s31", "-p", port, "-b", str(baud), "write-flash"]
    for slot, name in SLOTS:
        argv.extend([hex(slots["SLOT_" + slot]), str(images / name)])
    return argv


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, help="explicit mutable local build-tree verification mode")
    parser.add_argument("--artifact-dir", type=Path, help="published image set (default: dist/current)")
    parser.add_argument("--slot", choices=("all", "radio", "linux", "rootfs", "dtb", "uboot"), default="all")
    parser.add_argument("--existing", action="store_true", help="explicitly use current files; this helper never builds")
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=460800)
    parser.add_argument("--esptool", default="esptool")
    parser.add_argument("--dry-run", action="store_true", help="verify and print command without contacting hardware")
    args = parser.parse_args()
    try:
        if args.slot != "all":
            raise ValueError("partial flashing is disabled: installed companion identity is unknown; flash the verified all-slot set")
        root = (args.output_root or ROOT / "out").resolve()
        artifact_dir = args.artifact_dir
        if artifact_dir is None and args.output_root is None:
            artifact_dir = ROOT / "dist" / "current"
        argv = command(root, args.port, args.baud, args.esptool, artifact_dir=artifact_dir)
        if args.dry_run:
            print(shlex.join(argv))
        else:
            subprocess.run(argv, check=True)
    except (ValueError, OSError, KeyError) as error:
        parser.exit(1, "flash: " + str(error) + "\n")


if __name__ == "__main__":
    main()
