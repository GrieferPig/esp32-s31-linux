#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Link the board radio image and assemble flash through upstream genimage."""
import argparse
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

import radio_image

BOARDS = ("generic", "coreboard")
ARTIFACTS = ("spl_app.bin", "u-boot.itb", "xipImage", "rootfs.squashfs",
             "radio.bin", "radio.json")


def assemble_flash(boards):
    template = (Path(__file__).resolve().parents[1] / "genimage.cfg").read_text()
    flash_type, flash_template = template.split("\nimage ", maxsplit=1)
    configs = [flash_type]
    artifacts = []
    for board in boards:
        dtb = f"esp32s31-{board}.dtb"
        image = f"s31-{board}_full_flash.bin"
        configs.append("\nimage " + flash_template.replace("@FLASH_IMAGE@", image)
                       .replace("@BOARD_DTB@", dtb))
        artifacts.extend((dtb, image))
    with TemporaryDirectory(prefix="s31-genimage-") as temp:
        config = Path(temp) / "genimage.cfg"
        config.write_text("".join(configs))
        subprocess.run([os.environ["S31_GENIMAGE_SCRIPT"], "-c", str(config)], check=True)
    return artifacts


def finalize(output, board):
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
    if board == "wavesynth":
        fdtoverlay = Path(os.environ["HOST_DIR"]) / "bin/fdtoverlay"
        subprocess.run([fdtoverlay, "-i", images / "esp32s31-wavesynth-coreboard.dtb",
                        "-o", images / "esp32s31-wavesynth.dtb",
                        images / "esp32s31-overlay-i80-ili9341.dtbo"], check=True)
    board_artifacts = assemble_flash((board,) if board else BOARDS)
    with (images / "SHA256SUMS").open("w") as checksums:
        subprocess.run(["sha256sum", *ARTIFACTS, *board_artifacts], cwd=images,
                       stdout=checksums, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--board", choices=("korvo", "wavesynth"))
    args = parser.parse_args()
    finalize(args.output.resolve(), args.board)
