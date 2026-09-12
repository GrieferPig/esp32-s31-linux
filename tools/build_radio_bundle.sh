#!/bin/sh

set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
project_dir="$(CDPATH= cd -- "${script_dir}/.." && pwd)"
mode=engineering-only
grant=''
source_archive=''

usage()
{
	cat <<'EOF'
Usage:
  tools/build_radio_bundle.sh
  tools/build_radio_bundle.sh --release --grant FILE --source-archive FILE

The default creates an engineering-only binary bundle. Release mode requires
documented redistribution permission and an exact corresponding-source archive.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
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

make -C "$project_dir" radio-fs
make -C "$project_dir" build-manifest

kernel_out="${S31_LINUX_OUT:-${project_dir}/build/linux-6.18}"
radio_staging="${project_dir}/build/radiofs-staging"
radio_module="${radio_staging}/module/esp32s31-radio.ko.xz"
radio_firmware="${radio_staging}/firmware/esp32s31-radio-fw-v1.o.xz"
dtbo_dir="${kernel_out}/arch/riscv/boot/dts/espressif"
output_dir="${project_dir}/build/radio-package"
staging="$(mktemp -d "${project_dir}/build/.radio-package.XXXXXX")"
trap 'rm -rf "$staging"' EXIT

test -f "$radio_module"
test -f "$radio_firmware"
mkdir -p "$staging/module" "$staging/firmware" \
	"$staging/overlays" "$staging/config"
cp "$radio_module" "$staging/module/"
cp "$radio_firmware" "$staging/firmware/"
for overlay in radio-wifi radio-bluetooth radio-combo; do
	cp "${dtbo_dir}/esp32s31-overlay-${overlay}.dtbo" "$staging/overlays/"
done
cp "${project_dir}/firmware/radio/idf_deps/sdkconfig.defaults" "$staging/config/"
cp "${project_dir}/firmware/radio/idf_deps/sdkconfig.radio.defaults" "$staging/config/"
cp "${project_dir}/firmware/radio/RADIO_BUNDLE_LICENSES.md" "$staging/"
cp "${project_dir}/build/radio.sqfs" "$staging/"
cp "${project_dir}/build/build-manifest.json" "$staging/"
printf 'distribution-mode=%s\n' "$mode" >"$staging/MANIFEST"
printf 'kernel-release=%s\n' \
	"$(cat "${kernel_out}/include/config/kernel.release")" >>"$staging/MANIFEST"
printf 'radio-module-sha256=%s\n' \
	"$(sha256sum "$staging/module/esp32s31-radio.ko.xz" | awk '{print $1}')" \
	>>"$staging/MANIFEST"
printf 'external-firmware-sha256=%s\n' \
	"$(sha256sum "$staging/firmware/esp32s31-radio-fw-v1.o.xz" | awk '{print $1}')" \
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
