#!/bin/sh

set -eu

target_dir="$1"

# LP firmware is generated in the output tree. Overlay installation here runs after
# Buildroot's static overlay, replacing any legacy payload in an old target.
: "${S31_OVERLAY_DIR:?Set S31_OVERLAY_DIR to the build staging overlay}"
for firmware in s31-lp-core.elf s31-lp-core.bin; do
	[ -f "${S31_OVERLAY_DIR}/lib/firmware/esp32s31/${firmware}" ] || {
		echo "Missing staged LP firmware: ${firmware}" >&2
		exit 1
	}
done
cp -a "${S31_OVERLAY_DIR}/." "${target_dir}/"

chmod 0755 "${target_dir}/init"

# The cross-toolchain includes G++, but this compact image has no C++ target
# packages. Buildroot installs libstdc++ based on toolchain capability alone;
# omit that otherwise-unused runtime to keep the squashfs inside its partition.
rm -f \
	"${target_dir}"/lib/libstdc++.so* \
	"${target_dir}"/usr/lib/libgpiodcxx.so*

# Keep incremental builds honest after retiring the FreeRTOS hosted/audio
# transport. Buildroot does not uninstall files emitted by an older version of
# a local package when the install commands shrink.
rm -f \
	"${target_dir}/etc/init.d/S04s31-radio" \
	"${target_dir}/etc/init.d/S01s31-zram" \
	"${target_dir}/etc/init.d/S03s31-overlay" \
	"${target_dir}/etc/init.d/S35esp32-config" \
	"${target_dir}/usr/sbin/esp-hosted-ctl" \
	"${target_dir}/usr/sbin/s31-clock-compare" \
	"${target_dir}/usr/sbin/s31-cpufreq" \
	"${target_dir}/usr/sbin/s31-freertos-mem" \
	"${target_dir}/usr/sbin/wifi-connect" \
	"${target_dir}/usr/sbin/wifi-scan" \
	"${target_dir}/usr/sbin/ble-scan" \
	"${target_dir}/usr/sbin/test.out" \
	"${target_dir}/usr/bin/s31-audio-analyze" \
	"${target_dir}/usr/bin/s31-audio-loopback" \
	"${target_dir}/usr/bin/s31-audio-mic-test" \
	"${target_dir}/usr/bin/s31-audio-stream-stats"

# aplay/arecord use only the hardware PCM path on this image.  Keep ALSA's
# standard top-level configuration (it defines pcm.hw and ctl.hw), but omit
# card profiles, software PCM plugins, and initialization data that cannot be
# used by the ESP32-S31 I2S controller.
if [ -d "${target_dir}/usr/share/alsa" ]; then
	find "${target_dir}/usr/share/alsa" -mindepth 1 -maxdepth 1 \
		! -name alsa.conf -exec rm -rf {} +
fi
rm -f \
	"${target_dir}/usr/bin/aserver" \
	"${target_dir}"/usr/lib/libatopology.so*

# candump/cansend cover the normal SocketCAN receive/transmit path.  The rest
# of can-utils targets protocol labs, gateways, logging conversion, or test
# traffic generators; omit those from the 6 MiB production rootfs.
rm -f \
	"${target_dir}/usr/bin/asc2log" \
	"${target_dir}/usr/bin/bcmserver" \
	"${target_dir}/usr/bin/can-calc-bit-timing" \
	"${target_dir}/usr/bin/canbusload" \
	"${target_dir}/usr/bin/canfdtest" \
	"${target_dir}/usr/bin/cangen" \
	"${target_dir}/usr/bin/cangw" \
	"${target_dir}/usr/bin/canlogserver" \
	"${target_dir}/usr/bin/canplayer" \
	"${target_dir}/usr/bin/cansequence" \
	"${target_dir}/usr/bin/cansniffer" \
	"${target_dir}/usr/bin/isobusfs-cli" \
	"${target_dir}/usr/bin/isobusfs-srv" \
	"${target_dir}/usr/bin/isotpdump" \
	"${target_dir}/usr/bin/isotpperf" \
	"${target_dir}/usr/bin/isotprecv" \
	"${target_dir}/usr/bin/isotpsend" \
	"${target_dir}/usr/bin/isotpserver" \
	"${target_dir}/usr/bin/isotpsniffer" \
	"${target_dir}/usr/bin/isotptun" \
	"${target_dir}/usr/bin/j1939-timedate-cli" \
	"${target_dir}/usr/bin/j1939-timedate-srv" \
	"${target_dir}/usr/bin/j1939acd" \
	"${target_dir}/usr/bin/j1939cat" \
	"${target_dir}/usr/bin/j1939spy" \
	"${target_dir}/usr/bin/j1939sr" \
	"${target_dir}/usr/bin/log2asc" \
	"${target_dir}/usr/bin/log2long" \
	"${target_dir}/usr/bin/mcp251xfd-dump" \
	"${target_dir}/usr/bin/slcan_attach" \
	"${target_dir}/usr/bin/slcand" \
	"${target_dir}/usr/bin/slcanpty" \
	"${target_dir}/usr/bin/testj1939"

# PTP synchronization needs ptp4l plus phc2sys, while phc_ctl provides direct
# PHC read/set/frequency tests.  Omit orchestration and specialized timestamp
# tools from the compact image.  Explicitly remove ethtool/mmc too so an
# incremental Buildroot tree cannot retain packages removed from the defconfig.
rm -f \
	"${target_dir}/usr/sbin/ethtool" \
	"${target_dir}/etc/init.d/S65ptp4l" \
	"${target_dir}/etc/init.d/S66phc2sys" \
	"${target_dir}/usr/bin/mmc" \
	"${target_dir}/usr/sbin/hwstamp_ctl" \
	"${target_dir}/usr/sbin/nsm" \
	"${target_dir}/usr/sbin/pmc" \
	"${target_dir}/usr/sbin/timemaster" \
	"${target_dir}/usr/sbin/ts2phc" \
	"${target_dir}/usr/sbin/tz2alt"

# The HIL image uses i2cdetect/get/set/dump/transfer.  DDC/EDID, DIMM decode,
# EEPROM programming and stub reconstruction helpers do not exercise the S31
# controller and cost useful room in the fixed-size SquashFS partition.
rm -f \
	"${target_dir}/usr/bin/ddcmon" \
	"${target_dir}/usr/bin/decode-dimms" \
	"${target_dir}/usr/bin/decode-edid" \
	"${target_dir}/usr/bin/decode-vaio" \
	"${target_dir}/usr/sbin/eeprog" \
	"${target_dir}/usr/sbin/i2c-stub-from-dump"

# Trim tools and plugins that are not part of the compact appliance image.
rm -f \
	"${target_dir}/usr/bin/bluetoothctl" \
	"${target_dir}/usr/bin/btmgmt" \
	"${target_dir}/usr/bin/btgatt-client" \
	"${target_dir}/usr/bin/bluemoon" \
	"${target_dir}/usr/bin/btattach" \
	"${target_dir}/usr/bin/hex2hcd" \
	"${target_dir}/usr/bin/isotest" \
	"${target_dir}/usr/bin/l2ping" \
	"${target_dir}/usr/bin/l2test" \
	"${target_dir}/usr/bin/mpris-proxy" \
	"${target_dir}/usr/bin/rctest" \
	"${target_dir}/usr/bin/dbus-cleanup-sockets" \
	"${target_dir}/usr/bin/dbus-launch" \
	"${target_dir}/usr/bin/dbus-monitor" \
	"${target_dir}/usr/bin/dbus-run-session" \
	"${target_dir}/usr/bin/dbus-send" \
	"${target_dir}/usr/bin/dbus-test-tool" \
	"${target_dir}/usr/bin/dbus-update-activation-environment" \
	"${target_dir}/usr/bin/gapplication" \
	"${target_dir}/usr/bin/gdbus" \
	"${target_dir}/usr/bin/gi-compile-repository" \
	"${target_dir}/usr/bin/gi-decompile-typelib" \
	"${target_dir}/usr/bin/gi-inspect-typelib" \
	"${target_dir}/usr/bin/gio" \
	"${target_dir}/usr/bin/gio-querymodules" \
	"${target_dir}/usr/bin/gresource" \
	"${target_dir}/usr/bin/gsettings" \
	"${target_dir}/usr/bin/gpionotify" \
	"${target_dir}/usr/bin/pcre2grep" \
	"${target_dir}/usr/bin/pcre2test" \
	"${target_dir}/usr/bin/coremark" \
	"${target_dir}/usr/sbin/phc_ctl" \
	"${target_dir}/usr/sbin/segfault" \
	"${target_dir}/usr/sbin/forktest" \
	"${target_dir}/usr/sbin/membench" \
	"${target_dir}/usr/sbin/s31-libc-test" \
	"${target_dir}/usr/sbin/s31-mem-compare" \
	"${target_dir}/usr/sbin/s31-string-bench" \
	"${target_dir}/usr/libexec/gio-launch-desktop" \
	"${target_dir}"/usr/lib/libgirepository-2.0.so* \
	"${target_dir}"/usr/lib/libhistory.so* \
	"${target_dir}"/usr/lib/libreadline.so* \
	"${target_dir}"/usr/lib/libnl-xfrm-3.so* \
	"${target_dir}"/usr/lib/libnl-nf-3.so* \
	"${target_dir}"/usr/lib/libnl-idiag-3.so* \
	"${target_dir}"/usr/lib/libform.so* \
	"${target_dir}"/usr/lib/libmenu.so* \
	"${target_dir}"/usr/lib/libpanel.so* \
	"${target_dir}"/usr/lib/libisobusfs.so* \
	"${target_dir}"/usr/lib/libpcre2-posix.so* \
	"${target_dir}"/lib/libatomic.so* \
	"${target_dir}"/usr/lib/alsa-lib/libasound_module_ctl_bluealsa.so \
	"${target_dir}"/usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so \
	"${target_dir}/etc/alsa/conf.d/20-bluealsa.conf"
rm -rf \
	"${target_dir}/usr/share/glib-2.0/valgrind" \
	"${target_dir}/usr/share/gir-1.0"

# BlueZ and its D-Bus/GLib/BlueALSA closure are no longer selected. Purge files
# left in an incremental target directory so an old build cannot reintroduce a
# second Bluetooth host or its shared-library dependencies.
rm -f \
	"${target_dir}/etc/init.d/S30dbus" \
	"${target_dir}/etc/init.d/S30dbus-daemon" \
	"${target_dir}/etc/init.d/S40bluetoothd" \
	"${target_dir}/etc/init.d/S42s31-a2dp" \
	"${target_dir}/usr/bin/dbus-daemon" \
	"${target_dir}/usr/bin/dbus-uuidgen" \
	"${target_dir}/usr/bin/bluealsa" \
	"${target_dir}/usr/bin/bluealsa-aplay" \
	"${target_dir}/usr/sbin/s31-bt-agent" \
	"${target_dir}/usr/libexec/dbus-daemon-launch-helper" \
	"${target_dir}/usr/libexec/bluetooth/bluetoothd" \
	"${target_dir}"/usr/lib/libbluetooth.so* \
	"${target_dir}"/usr/lib/libdbus-1.so* \
	"${target_dir}"/usr/lib/libgio-2.0.so* \
	"${target_dir}"/usr/lib/libglib-2.0.so* \
	"${target_dir}"/usr/lib/libgmodule-2.0.so* \
	"${target_dir}"/usr/lib/libgobject-2.0.so* \
	"${target_dir}"/usr/lib/libgthread-2.0.so*
rm -rf \
	"${target_dir}/etc/bluetooth" \
	"${target_dir}/etc/dbus-1" \
	"${target_dir}/usr/lib/gio" \
	"${target_dir}/usr/share/dbus-1" \
	"${target_dir}/usr/share/xml/dbus-1" \
	"${target_dir}/usr/share/glib-2.0" \
	"${target_dir}/var/lib/bluetooth" \
	"${target_dir}/var/lib/dbus"

# Full builds retain the normal Buildroot logging and scheduled-job services.

rm -rf \
	"${target_dir}/tmp" \
	"${target_dir}/run" \
	"${target_dir}/var/log" \
	"${target_dir}/var/tmp"

mkdir -m 1777 "${target_dir}/tmp"
mkdir -m 0755 "${target_dir}/run" "${target_dir}/var/log"
ln -s /tmp "${target_dir}/var/tmp"

# Pairing keys live on the persistent merged upper layer.
mkdir -m 0700 -p "${target_dir}/var/lib/btstack"

rm -rf "${target_dir}/var/lib/seedrng"
ln -s /run/seedrng "${target_dir}/var/lib/seedrng"

rm -f "${target_dir}/etc/mtab" "${target_dir}/etc/resolv.conf"
ln -s /proc/mounts "${target_dir}/etc/mtab"
ln -s /run/resolv.conf "${target_dir}/etc/resolv.conf"
chmod 0600 "${target_dir}/etc/wpa_supplicant.conf"

: "${S31_DTBO_DIR:?Set S31_DTBO_DIR to the build kernel DT overlay directory}"
dtbo_dir="${S31_DTBO_DIR}"
install_dir="${target_dir}/usr/lib/s31-overlays"

mkdir -p "${install_dir}"
for dtbo in "${dtbo_dir}"/esp32s31-overlay-*.dtbo; do
	[ -f "${dtbo}" ] || {
		echo "Missing S31 DT overlays in ${dtbo_dir}" >&2
		exit 1
	}
	cp "${dtbo}" "${install_dir}/"
done

# Only the XIP radio module is installed in rootfs.
find "${target_dir}/lib/modules" -type f \
	\( -name 'esp32s31-radio*.ko' -o -name 'esp32s31-wifi.ko' -o \
	   -name 'esp32s31-btdm.ko' \) -delete 2>/dev/null || true
[ -f "${S31_RADIO_MODULE:-}" ] || { echo 'Missing XIP radio module' >&2; exit 1; }
mkdir -p "${target_dir}/usr/lib/s31-radio"
cp "${S31_RADIO_MODULE}" "${target_dir}/usr/lib/s31-radio/esp32s31-radio.ko"
"${HOST_DIR}/bin/riscv32-esp-linux-musl-strip" --strip-debug \
	"${target_dir}/usr/lib/s31-radio/esp32s31-radio.ko"
xz --check=crc32 --lzma2=dict=64KiB -f "${target_dir}/usr/lib/s31-radio/esp32s31-radio.ko"
rm -f "${target_dir}/usr/lib/s31-radio/s31-dma-ccm-net.ko.xz" \
	"${target_dir}/etc/esp32-conf/softmac.conf"

required_runtime='usr/sbin/s31-btstack-a2dp usr/sbin/s31-ext-test'

for required in ${required_runtime}; do
	[ -x "${target_dir}/${required}" ] || {
		echo "Missing required runtime: /${required}" >&2
		exit 1
	}
done
