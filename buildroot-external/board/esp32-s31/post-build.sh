#!/bin/sh
# Generate configuration-menu capabilities from the built kernel.
set -eu
target_dir="$1"

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
: "${S31_KERNEL_CONFIG:?Set S31_KERNEL_CONFIG to the build kernel configuration}"
: "${HOST_DIR:?Buildroot must provide HOST_DIR for board assets}"
"${HOST_DIR}/bin/python3" "${script_dir}/scripts/build_esp32_config_assets.py" \
	--target-dir "$target_dir" \
	--kernel-config "$S31_KERNEL_CONFIG"

if grep -qx 'CONFIG_FB_ESP32S31_I80=y' "$S31_KERNEL_CONFIG"; then
	lcd_getty='tty1::respawn:/sbin/getty 38400 tty1 linux'
	if ! grep -qxF "$lcd_getty" "$target_dir/etc/inittab"; then
		printf '%s\n' "$lcd_getty" >> "$target_dir/etc/inittab"
	fi
fi
