#!/bin/sh

set -eu

target_dir="$1"

chmod 0755 "${target_dir}/init"

# The cross-toolchain includes G++, but this compact image has no C++ target
# packages. Buildroot installs libstdc++ based on toolchain capability alone;
# omit that otherwise-unused runtime to keep the squashfs inside its partition.
rm -f "${target_dir}"/lib/libstdc++.so*

# Keep incremental builds honest after retiring the FreeRTOS hosted/audio
# transport. Buildroot does not uninstall files emitted by an older version of
# a local package when the install commands shrink.
rm -f \
	"${target_dir}/usr/sbin/esp-hosted-ctl" \
	"${target_dir}/usr/sbin/s31-clock-compare" \
	"${target_dir}/usr/sbin/s31-cpufreq" \
	"${target_dir}/usr/sbin/s31-freertos-mem" \
	"${target_dir}/usr/sbin/test.out" \
	"${target_dir}/usr/bin/s31-audio-analyze" \
	"${target_dir}/usr/bin/s31-audio-loopback" \
	"${target_dir}/usr/bin/s31-audio-mic-test" \
	"${target_dir}/usr/bin/s31-audio-stream-stats" \
	"${target_dir}/usr/bin/aplay" \
	"${target_dir}/usr/bin/arecord"
rm -rf "${target_dir}/usr/share/alsa"
rm -f "${target_dir}"/usr/lib/libasound.so*

rm -rf \
	"${target_dir}/tmp" \
	"${target_dir}/run" \
	"${target_dir}/var/log" \
	"${target_dir}/var/tmp"

mkdir -m 1777 "${target_dir}/tmp"
mkdir -m 0755 "${target_dir}/run" "${target_dir}/var/log"
ln -s /tmp "${target_dir}/var/tmp"

rm -rf "${target_dir}/var/lib/bluetooth"
ln -s /run/bluetooth "${target_dir}/var/lib/bluetooth"

rm -rf "${target_dir}/var/lib/seedrng"
ln -s /run/seedrng "${target_dir}/var/lib/seedrng"

rm -f "${target_dir}/etc/mtab" "${target_dir}/etc/resolv.conf"
ln -s /proc/mounts "${target_dir}/etc/mtab"
ln -s /run/resolv.conf "${target_dir}/etc/resolv.conf"

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
project_dir="$(CDPATH= cd -- "${script_dir}/../../.." && pwd)"
dtbo_dir="${project_dir}/build/linux/arch/riscv/boot/dts/espressif"
install_dir="${target_dir}/usr/lib/s31-overlays"

mkdir -p "${install_dir}"
for dtbo in "${dtbo_dir}"/esp32s31-overlay-*.dtbo; do
	[ -f "${dtbo}" ] || {
		echo "Missing S31 DT overlays in ${dtbo_dir}" >&2
		exit 1
	}
	cp "${dtbo}" "${install_dir}/"
done
