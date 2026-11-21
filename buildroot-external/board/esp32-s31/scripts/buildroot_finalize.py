#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Link the board radio image and assemble flash through upstream genimage."""
import argparse
import os
from pathlib import Path
import subprocess
import radio_image

ARTIFACTS = ("spl_app.bin", "u-boot.itb", "esp32s31-i80-ili9341.dtb", "xipImage",
             "rootfs.squashfs", "radio.bin", "radio.json", "s31_full_flash.bin")


def finalize(output):
    images = output / "images"
    kernel = Path(os.environ["S31_LINUX_DIR"])
    payload = Path(os.environ["S31_FIRMWARE_DIR"])
    args = argparse.Namespace(
        prefix=os.environ["S31_CROSS_COMPILE"], kernel=kernel / "vmlinux",
        payload=payload / "radio/linux_radio.localized.o",
        imports=payload / "radio/linux-radio-linked-imports.txt",
        sram_symbols=payload / "source/firmware/radio/wifi_sram_symbols.txt",
        output=images / "radio.bin")
    radio_image.build(args)
    subprocess.run([os.environ["S31_GENIMAGE_SCRIPT"], "-c",
                    str(Path(__file__).resolve().parents[1] / "genimage.cfg")], check=True)
    with (images / "SHA256SUMS").open("w") as checksums:
        subprocess.run(["sha256sum", *ARTIFACTS], cwd=images,
                       stdout=checksums, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    finalize(parser.parse_args().output.resolve())
