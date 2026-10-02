#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 \
	"${test_dir}/test_gpio.c" -o "${temp_dir}/test-gpio"
"${temp_dir}/test-gpio"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Os \
	"${test_dir}/../../rootfs/s31_gpio.c" -o "${temp_dir}/s31-gpio"
printf '4 input down\n5 high none\n' >"${temp_dir}/valid.conf"
"${temp_dir}/s31-gpio" check "${temp_dir}/valid.conf"
printf '26 high none\n' >"${temp_dir}/invalid.conf"
if "${temp_dir}/s31-gpio" check "${temp_dir}/invalid.conf" 2>/dev/null; then
	echo 'Reserved GPIO was accepted by the command-line validator.' >&2
	exit 1
fi
sh -n "${test_dir}/../../buildroot-external/board/esp32-s31/overlay/usr/lib/esp32-config/gpio.sh"
sh -n "${test_dir}/../../buildroot-external/board/esp32-s31/overlay/etc/init.d/S46s31-gpio"
sh "${test_dir}/test-gpio-menu.sh"
