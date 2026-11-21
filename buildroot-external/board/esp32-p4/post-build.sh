#!/bin/sh

set -eu

target_dir="$1"

chmod 0755 "${target_dir}/etc/init.d/S35esp-hosted"
chmod 0755 "${target_dir}/usr/sbin/p4-soak"

# Keep association and address assignment manual during bring-up.  The
# ESP-Hosted module is loaded by S35esp-hosted before an operator uses iw.
rm -f "${target_dir}/etc/init.d/S40network"

# The read-only root filesystem has no persistent seed file and BusyBox omits seedrng.
rm -f "${target_dir}/etc/init.d/S01seedrng"

# Buildroot's stock udhcpc hook starts another BusyBox process solely to
# allocate a temporary resolv.conf.  On NOMMU that duplicate image needs a
# large contiguous allocation and can fail once wpa_supplicant is resident.
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

# devtmpfs is mounted before BusyBox init and starts without /dev/pts.
inittab="$target_dir/etc/inittab"
mount_cmd='::sysinit:/bin/mount -a'
mkdir_pts='::sysinit:/bin/mkdir -p /dev/pts'
if ! grep -qxF "$mkdir_pts" "$inittab"; then
	grep -qxF "$mount_cmd" "$inittab"
	sed -i "/^::sysinit:.*mount -a$/i $mkdir_pts" "$inittab"
fi
