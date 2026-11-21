#!/bin/sh

set -eu

target_dir="$1"

# Buildroot retains files from disabled packages and removed overlays in incremental builds.
rm -f \
	"${target_dir}/usr/bin/coremark" \
	"${target_dir}/usr/sbin/esp-periph-test" \
	"${target_dir}/usr/sbin/p4-soak" \
	"${target_dir}/usr/sbin/wpa_supplicant" \
	"${target_dir}/usr/sbin/wpa_cli" \
	"${target_dir}/etc/wpa_supplicant.conf" \
	"${target_dir}/etc/network/if-up.d/wpasupplicant" \
	"${target_dir}/etc/network/if-down.d/wpasupplicant"

# Keep association and address assignment manual during bring-up.
rm -f "${target_dir}/etc/init.d/S40network"

# The read-only root filesystem has no persistent seed file.
rm -f "${target_dir}/etc/init.d/S01seedrng"

# Buildroot's stock udhcpc hook starts another BusyBox process solely to
# allocate a temporary resolv.conf.  On NOMMU that duplicate image needs a
# large contiguous allocation even for a temporary filename.
# A per-hook PID suffix is sufficient here and avoids the extra exec.
udhcpc_script="${target_dir}/usr/share/udhcpc/default.script"
if [ -f "${udhcpc_script}" ]; then
	sed -i 's|TMPFILE=$(mktemp)|TMPFILE="${RESOLV_CONF}.udhcpc.$$"|g' \
		"${udhcpc_script}"
fi

rm -rf \
	"${target_dir}/tmp" \
	"${target_dir}/run" \
	"${target_dir}/var/log" \
	"${target_dir}/var/tmp"

mkdir -m 1777 "${target_dir}/tmp"
mkdir -m 0755 "${target_dir}/run" "${target_dir}/var/log"
ln -s /tmp "${target_dir}/var/tmp"

fstab="${target_dir}/etc/fstab"
for mountpoint in /run /tmp /var/log; do
	if ! awk -v path="${mountpoint}" '$2 == path { found = 1 } END { exit !found }' "${fstab}"; then
		mode=0755
		if [ "${mountpoint}" = /tmp ]; then
			mode=1777
		fi
		printf 'tmpfs %s tmpfs mode=%s,nosuid,nodev 0 0\n' "${mountpoint}" "${mode}" >> "${fstab}"
	fi
done
