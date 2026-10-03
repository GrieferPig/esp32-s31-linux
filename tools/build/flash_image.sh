#!/bin/bash
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-FileCopyrightText: 2026 GrieferPig
#
# SPDX-License-Identifier: Apache-2.0
#
# Derived from Espressif esp-linux-bsp commit
# a77a06f03068d25799ddd719566df50207f28c5a.
# Modified for the SMP/XIP/SquashFS/persistent-storage layout.

# Merge the ESP32-S31 U-Boot/Linux NOR image from one board layout.

set -euo pipefail

CFG="${1:?usage: tools/build/flash_image.sh <layout.cfg> <images_dir>}"
IMAGES_DIR="${2:?usage: tools/build/flash_image.sh <layout.cfg> <images_dir>}"

[ -f "$CFG" ] || { echo "ERROR: layout cfg not found: $CFG" >&2; exit 1; }
# shellcheck disable=SC1090
source "$CFG"

: "${CHIP:?CHIP not set in $CFG}"
: "${ESPTOOL_FLASH:?ESPTOOL_FLASH not set in $CFG}"
: "${SLOT_SPL:?SLOT_SPL not set in $CFG}"
: "${SIZE_SPL:?SIZE_SPL not set in $CFG}"
: "${SLOT_UBOOT_ITB:?SLOT_UBOOT_ITB not set in $CFG}"
: "${SLOT_DTB:?SLOT_DTB not set in $CFG}"
: "${SLOT_RADIO:?SLOT_RADIO not set in $CFG}"
: "${SLOT_KERNEL:?SLOT_KERNEL not set in $CFG}"
: "${SLOT_PERSIST:?SLOT_PERSIST not set in $CFG}"
: "${SLOT_ROOTFS:?SLOT_ROOTFS not set in $CFG}"
: "${FLASH_SIZE:?FLASH_SIZE not set in $CFG}"

: "${SPL_APP_BIN:=spl_app.bin}"
: "${UBOOT_ITB:=u-boot.itb}"
: "${BASE_DTB:=esp32s31_generic.dtb}"
: "${RADIO_IMAGE:=radio.bin}"
: "${KERNEL_IMAGE:=xipImage}"
: "${ROOTFS_IMAGE:=rootfs.sqfs}"
: "${OUT_IMAGE:=s31_full_flash.bin}"

if [ -n "${ESP_ESPTOOL:-}" ]; then
	ESPTOOL="$ESP_ESPTOOL"
elif [ -n "${HOST_DIR:-}" ] && [ -x "$HOST_DIR/bin/esptool" ]; then
	ESPTOOL="$HOST_DIR/bin/esptool"
elif [ -n "${HOST_DIR:-}" ] && [ -x "$HOST_DIR/bin/esptool.py" ]; then
	ESPTOOL="$HOST_DIR/bin/esptool.py"
else
	ESPTOOL=esptool
fi
command -v "$ESPTOOL" >/dev/null 2>&1 || {
	echo "ERROR: esptool not found (set ESP_ESPTOOL or source ESP-IDF)" >&2
	exit 1
}

cd "$IMAGES_DIR"
for image in "$SPL_APP_BIN" "$UBOOT_ITB" "$BASE_DTB" \
	     "$RADIO_IMAGE" "$KERNEL_IMAGE" "$ROOTFS_IMAGE"; do
	[ -f "$image" ] || { echo "ERROR: required input missing: $IMAGES_DIR/$image" >&2; exit 1; }
done

check_slot()
{
	local image=$1 start=$2 end=$3 size capacity
	size=$(stat -c%s "$image")
	capacity=$((end - start))
	if (( size == 0 || size > capacity )); then
		echo "ERROR: $image is $size bytes, slot capacity is $capacity bytes" >&2
		exit 1
	fi
}

check_slot "$SPL_APP_BIN" "$((SLOT_SPL))" "$((SLOT_SPL + SIZE_SPL))"
check_slot "$UBOOT_ITB" "$((SLOT_UBOOT_ITB))" "$((SLOT_DTB))"
check_slot "$BASE_DTB" "$((SLOT_DTB))" "$((SLOT_RADIO))"
check_slot "$RADIO_IMAGE" "$((SLOT_RADIO))" "$((SLOT_PERSIST))"
check_slot "$KERNEL_IMAGE" "$((SLOT_KERNEL))" "$((SLOT_ROOTFS))"
check_slot "$ROOTFS_IMAGE" "$((SLOT_ROOTFS))" "$((FLASH_SIZE))"

# Persist has no input payload, but combined-image padding overwrites it.
# Only slot-wise flash-all on the same layout preserves existing user data.
# shellcheck disable=SC2086
"$ESPTOOL" --chip "$CHIP" merge-bin -o "$OUT_IMAGE" --format raw \
	$ESPTOOL_FLASH \
	"$SLOT_SPL" "$SPL_APP_BIN" \
	"$SLOT_UBOOT_ITB" "$UBOOT_ITB" \
	"$SLOT_DTB" "$BASE_DTB" \
	"$SLOT_RADIO" "$RADIO_IMAGE" \
	"$SLOT_KERNEL" "$KERNEL_IMAGE" \
	"$SLOT_ROOTFS" "$ROOTFS_IMAGE"

echo "$OUT_IMAGE ($(stat -c%s "$OUT_IMAGE") bytes) ready in $IMAGES_DIR"
