#!/bin/sh
set -eu

images="$1"
board_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

exec python3 "$board_dir/pack-fit.py" --mkimage "$HOST_DIR/bin/mkimage" \
	--rootfs "$images/rootfs.squashfs" \
	"$images/spl-p4-200.bin" "$images/uboot-p4.bin" \
	"$images/xipImage" "$images/esp32p4-waveshare-wifi6-dev-kit.dtb" \
	"$images/fit-200"
