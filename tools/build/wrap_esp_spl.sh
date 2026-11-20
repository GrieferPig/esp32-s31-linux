#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Turn an SRAM-linked raw SPL into an image for the selected ESP mask ROM.
set -eu
if [ "$#" -lt 8 ]; then
	echo "usage: $0 <esptool> <objcopy> <chip> <entry> <mode> <freq> <raw-spl> <output> [elf2image options]" >&2
	exit 2
fi
esptool=$1 objcopy=$2 chip=$3 entry=$4 mode=$5 freq=$6 raw_spl=$7 output=$8
shift 8
work=$(mktemp -d "${output}.wrap.XXXXXX")
trap 'rm -rf "$work"' EXIT
"$objcopy" -I binary -O elf32-littleriscv -B riscv "$raw_spl" "$work/spl.elf"
"$objcopy" --change-section-address ".data=$entry" \
	--rename-section .data=.text,alloc,load,readonly,code,contents \
	--set-start "$entry" "$work/spl.elf" "$work/spl_wrapped.elf"
"$esptool" --chip "$chip" elf2image --flash-mode "$mode" --flash-freq "$freq" \
	--flash-size 16MB "$@" --output "$work/spl.bin" "$work/spl_wrapped.elf"
test -s "$work/spl.bin"
mv "$work/spl.bin" "$output"
