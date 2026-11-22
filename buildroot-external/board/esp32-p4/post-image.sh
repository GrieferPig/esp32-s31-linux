#!/bin/sh
set -eu

images="$1"
board_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

for board in generic waveshare-wifi6-dev-kit; do
	out="$images/fit-200/$board"
	python3 "$board_dir/pack-fit.py" --mkimage "$HOST_DIR/bin/mkimage" \
		--rootfs "$images/rootfs.squashfs" \
		"$images/spl-p4-200.bin" "$images/uboot-p4.bin" \
		"$images/xipImage" "$images/esp32p4-$board.dtb" "$out"
	mv "$out/p4_full_flash.bin" "$images/p4-${board}_full_flash.bin"
done
