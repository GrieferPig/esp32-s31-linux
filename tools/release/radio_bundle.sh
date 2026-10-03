#!/bin/sh
# Package existing, verified outputs only. Builds belong to the top-level graph.
set -eu
project_dir="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
output_root=${S31_OUTPUT_ROOT:-}
mode=engineering-only
grant=''
source_archive=''
usage()
{
	cat <<'EOF'
Usage: tools/release/radio_bundle.sh [--output-root DIR]
       [--release --grant FILE --source-archive FILE]

Packages a verified existing full build. Run make all first.
Release mode requires a redistribution grant and corresponding-source archive.
EOF
}
while [ "$#" -gt 0 ]; do
	case "$1" in
	--output-root) output_root="$2"; shift 2 ;;
	--existing-build) shift ;; # All invocations are now packaging-only.
	--release) mode=release; shift ;;
	--grant) grant="$2"; shift 2 ;;
	--source-archive) source_archive="$2"; shift 2 ;;
	-h|--help) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done
output_root=${output_root:-${project_dir}/out}
images=${output_root}/images
kernel_out=${output_root}/linux
if [ "$mode" = release ]; then
	[ -s "$grant" ] || { echo 'release mode requires --grant FILE' >&2; exit 1; }
	[ -s "$source_archive" ] || { echo 'release mode requires --source-archive FILE' >&2; exit 1; }
fi
python3 "${project_dir}/tools/release/manifest.py" \
	--output-root "$output_root" --verify "${images}/build-manifest.json"
mkdir -p "${output_root}/staging" "$images"
staging=$(mktemp -d "${output_root}/staging/.radio-package.XXXXXX")
archive="${images}/esp32s31-radio-${mode}.tar.xz"
temporary=$(mktemp "${images}/.radio-package.XXXXXX")
trap 'rm -rf "$staging"; rm -f "$temporary"' EXIT HUP INT TERM
mkdir -p "$staging/module" "$staging/firmware" "$staging/overlays" "$staging/config"
cp "${kernel_out}/drivers/platform/esp32s31-radio.ko" "$staging/module/"
# Keep the exact paired module bytes; packaging must not relink or strip them.
xz -f --check=crc32 --lzma2=dict=64KiB "$staging/module/esp32s31-radio.ko"
cp "${images}/radio.bin" "${images}/radio.json" "$staging/firmware/"
for overlay in radio-wifi radio-bluetooth radio-combo; do
	cp "${kernel_out}/arch/riscv/boot/dts/espressif/esp32s31-overlay-${overlay}.dtbo" "$staging/overlays/"
done
cp "${project_dir}/firmware/radio/idf_deps/sdkconfig.defaults" "$staging/config/"
cp "${project_dir}/firmware/radio/idf_deps/sdkconfig.radio.defaults" "$staging/config/"
cp "${project_dir}/firmware/radio/RADIO_BUNDLE_LICENSES.md" "$staging/"
cp "${images}/build-manifest.json" "$staging/"
printf 'distribution-mode=%s\nradio-storage=flash-xip\n' "$mode" >"$staging/MANIFEST"
printf 'kernel-release=%s\n' "$(cat "${kernel_out}/include/config/kernel.release")" >>"$staging/MANIFEST"
printf 'radio-module-sha256=%s\n' "$(sha256sum "$staging/module/esp32s31-radio.ko.xz" | awk '{print $1}')" >>"$staging/MANIFEST"
printf 'external-firmware-sha256=%s\n' "$(sha256sum "$staging/firmware/radio.bin" | awk '{print $1}')" >>"$staging/MANIFEST"
if [ "$mode" = release ]; then
	cp "$grant" "$staging/REDISTRIBUTION_GRANT"
	cp "$source_archive" "$staging/CORRESPONDING_SOURCE"
fi
python3 "${project_dir}/tools/release/manifest.py" \
	--output-root "$output_root" --verify-radio-package "$staging"
(cd "$staging" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum >SHA256SUMS)
tar -C "$staging" -cJf "$temporary" .
mv -f "$temporary" "$archive"
echo "Radio bundle: $archive"
