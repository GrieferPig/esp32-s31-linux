#!/bin/sh
# Read ESP32-S31 flash in restartable chunks to tolerate marginal USB-UART links.
set -eu

port=${1:-/dev/ttyUSB0}
output=${2:-out/reports/backup-flash.bin}
chunk_dir="${output}.chunks"

mkdir -p "$chunk_dir"
i=0
while [ "$i" -lt 16 ]; do
	offset=$((i * 0x100000))
	chunk=$(printf '%s/%02d.bin' "$chunk_dir" "$i")
	try=1
	ok=0
	while [ "$try" -le 3 ]; do
		printf 'backup chunk %d/15 offset 0x%06x attempt %d\n' \
			"$i" "$offset" "$try"
		if esptool -p "$port" -b 460800 read-flash \
			"$offset" 0x100000 "$chunk" &&
		   [ "$(stat -c%s "$chunk")" -eq 1048576 ]; then
			ok=1
			break
		fi
		try=$((try + 1))
	done
	[ "$ok" -eq 1 ] || exit 1
	i=$((i + 1))
done

truncate -s 16777216 "$output"
i=0
while [ "$i" -lt 16 ]; do
	chunk=$(printf '%s/%02d.bin' "$chunk_dir" "$i")
	dd if="$chunk" of="$output" bs=1048576 seek="$i" conv=notrunc status=none
	i=$((i + 1))
done
sha256sum "$output"
