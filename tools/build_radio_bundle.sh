#!/bin/sh

set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
project_dir="$(CDPATH= cd -- "${script_dir}/.." && pwd)"
mode=engineering-only
grant=''
source_archive=''
existing_build=0

usage()
{
	cat <<'EOF'
Usage:
  tools/build_radio_bundle.sh [--existing-build]
  tools/build_radio_bundle.sh --release --grant FILE --source-archive FILE

The default creates an engineering-only binary bundle. Release mode requires
documented redistribution permission and an exact corresponding-source archive.
--existing-build packages existing images without rebuilding them.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--existing-build) existing_build=1; shift ;;
	--release) mode=release; shift ;;
	--grant) grant="$2"; shift 2 ;;
	--source-archive) source_archive="$2"; shift 2 ;;
	-h|--help) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done

if [ "$mode" = release ]; then
	[ -f "$grant" ] || {
		echo 'release mode requires --grant FILE' >&2
		exit 1
	}
	[ -f "$source_archive" ] || {
		echo 'release mode requires --source-archive FILE' >&2
		exit 1
	}
fi

if [ "$existing_build" != 1 ]; then
	make -C "$project_dir" radio-fs
fi
make -C "$project_dir" build-manifest

kernel_out="${S31_LINUX_OUT:-${project_dir}/build/linux-6.18}"
dtbo_dir="${kernel_out}/arch/riscv/boot/dts/espressif"
output_dir="${project_dir}/build/radio-package"
staging="$(mktemp -d "${project_dir}/build/.radio-package.XXXXXX")"
trap 'rm -rf "$staging"' EXIT

mkdir -p "$staging/module" "$staging/firmware" \
	"$staging/overlays" "$staging/config"
	cp "${kernel_out}/drivers/platform/esp32s31-radio.ko" "$staging/module/"
	"${CROSS_COMPILE:-${project_dir}/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-}strip" \
		--strip-debug "$staging/module/esp32s31-radio.ko"
	xz -f --check=crc32 --lzma2=dict=64KiB "$staging/module/esp32s31-radio.ko"
	cp "${project_dir}/build/radio.bin" "$staging/firmware/"
	cp "${project_dir}/build/radio.json" "$staging/firmware/"
	firmware_name=radio.bin
	storage=flash-xip
for overlay in radio-wifi radio-bluetooth radio-combo; do
	cp "${dtbo_dir}/esp32s31-overlay-${overlay}.dtbo" "$staging/overlays/"
done
cp "${project_dir}/firmware/radio/idf_deps/sdkconfig.defaults" "$staging/config/"
cp "${project_dir}/firmware/radio/idf_deps/sdkconfig.radio.defaults" "$staging/config/"
cp "${project_dir}/firmware/radio/RADIO_BUNDLE_LICENSES.md" "$staging/"
cp "${project_dir}/build/build-manifest.json" "$staging/"
printf 'distribution-mode=%s\n' "$mode" >"$staging/MANIFEST"
printf 'radio-storage=%s\n' "$storage" >>"$staging/MANIFEST"
printf 'kernel-release=%s\n' \
	"$(cat "${kernel_out}/include/config/kernel.release")" >>"$staging/MANIFEST"
printf 'radio-module-sha256=%s\n' \
	"$(sha256sum "$staging/module/esp32s31-radio.ko.xz" | awk '{print $1}')" \
	>>"$staging/MANIFEST"
printf 'external-firmware-sha256=%s\n' \
	"$(sha256sum "$staging/firmware/$firmware_name" | awk '{print $1}')" \
	>>"$staging/MANIFEST"
if [ "$mode" = release ]; then
	cp "$grant" "$staging/REDISTRIBUTION_GRANT"
	cp "$source_archive" "$staging/CORRESPONDING_SOURCE"
fi
(cd "$staging" && \
	find . -type f ! -name SHA256SUMS -print0 | sort -z | \
	xargs -0 sha256sum >SHA256SUMS)
mkdir -p "$output_dir"
archive="${output_dir}/esp32s31-radio-${mode}.tar.xz"
rm -f "$archive"
tar -C "$staging" -cJf "$archive" .
echo "Radio bundle: $archive"
