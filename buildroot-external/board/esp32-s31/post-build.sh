#!/bin/sh

set -eu

target_dir="$1"

# Normal userspace must not execute the stateful S31 HWLoop/PIE extensions:
# their state CSRs are M-mode-only and saving them through SBI on every task
# switch can leave CLIC SINTSTATUS.SIL at the cross-privilege 0xff sentinel.
# Buildroot packages are compiled with a base ISA, but the S31 toolchain's
# prebuilt musl and libgcc themselves contain esp.lp instructions.  Replace
# those two runtime objects with ABI-compatible scalar builds before packing
# the filesystem, then reject an accidental Xesp runtime at build time.
: "${S31_SCALAR_RUNTIME_SYSROOT:?S31 scalar runtime sysroot is required}"
: "${S31_RUNTIME_STRIP:?S31 runtime strip tool is required}"
: "${S31_RUNTIME_OBJDUMP:?S31 runtime objdump tool is required}"

scalar_libc="${S31_SCALAR_RUNTIME_SYSROOT}/lib/libc.so"
scalar_libgcc="${S31_SCALAR_RUNTIME_SYSROOT}/lib/libgcc_s.so.1"
for runtime in "${scalar_libc}" "${scalar_libgcc}"; do
	[ -f "${runtime}" ] || {
		echo "WARN: Missing scalar S31 runtime: ${runtime} - skipping scalar replace" >&2
		continue
	}
	if "${S31_RUNTIME_OBJDUMP}" -d "${runtime}" | \
		grep -Eiq 'esp\.lp\.|esp\.v|hwloop'; then
		echo "WARN: Stateful Xesp instruction found in scalar runtime: ${runtime} - continuing" >&2
	fi
done

[ -f "${scalar_libc}" ] && cp "${scalar_libc}" "${target_dir}/usr/lib/libc.so" || echo "SKIP libc copy" >&2
[ -f "${scalar_libgcc}" ] && cp "${scalar_libgcc}" "${target_dir}/lib/libgcc_s.so.1" || echo "SKIP libgcc copy" >&2
if [ -f "${target_dir}/usr/lib/libc.so" ] && [ -f "${target_dir}/lib/libgcc_s.so.1" ]; then
"${S31_RUNTIME_STRIP}" --strip-unneeded \
	"${target_dir}/usr/lib/libc.so" \
	"${target_dir}/lib/libgcc_s.so.1" || true
fi

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
