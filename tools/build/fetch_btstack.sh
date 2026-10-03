#!/bin/sh

set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
version=$(sed -n 's/^BTSTACK_REF[[:space:]]*[:?]*=[[:space:]]*//p' "${project_dir}/configs/build-versions.mk")
[ -n "$version" ] || { echo 'Missing BTSTACK_REF dependency lock' >&2; exit 1; }
build_dir=$(realpath -m "${S31_CACHE_ROOT:-${project_dir}/cache}/sources")
output_dir=${1:-${build_dir}/btstack}
resolved_output=$(realpath -m "$output_dir")
case "$resolved_output" in
	"${build_dir}"/*) ;;
	*) echo "Refusing BTstack source output outside ${build_dir}: ${resolved_output}" >&2; exit 1 ;;
esac
mkdir -p "$build_dir"
exec 9>"${build_dir}/.btstack.lock"
flock 9

if [ -r "${resolved_output}/.s31-btstack-version" ] &&
   [ "$(cat "${resolved_output}/.s31-btstack-version")" = "$version" ] &&
   [ -f "${resolved_output}/example/a2dp_sink_demo.c" ] &&
   [ -f "${resolved_output}/platform/linux/hci_transport_linux.c" ]; then
	exit 0
fi

if [ -e "$resolved_output" ]; then
	echo "Refusing to replace an unverified existing source tree: $resolved_output" >&2
	exit 1
fi

mkdir -p "$build_dir"
temporary=$(mktemp -d "${build_dir}/btstack-source.XXXXXX")
repository=${temporary}/repository
staged=${temporary}/staged
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

git -C "$temporary" init -q repository
git -C "$repository" remote add origin https://github.com/bluekitchen/btstack.git
git -C "$repository" config core.sparseCheckoutCone true
git -C "$repository" sparse-checkout set \
	3rd-party/bluedroid/decoder \
	3rd-party/bluedroid/encoder \
	3rd-party/lc3-google/include \
	3rd-party/md5 \
	3rd-party/yxml \
	example \
	platform/embedded \
	platform/linux \
	platform/posix \
	port/linux \
	src

attempt=1
while ! git -C "$repository" -c http.version=HTTP/1.1 fetch -q \
	--depth=1 --filter=blob:none origin "$version"; do
	[ "$attempt" -lt 3 ] || {
		echo "Unable to fetch BTstack commit ${version} after ${attempt} attempts" >&2
		exit 1
	}
	attempt=$((attempt + 1))
done
git -C "$repository" checkout -q --detach FETCH_HEAD

mkdir -p "$staged"
(cd "$repository" && tar --exclude=.git -cf - .) |
	(cd "$staged" && tar -xf -)
printf '%s\n' "$version" >"${staged}/.s31-btstack-version"

mkdir -p "$(dirname -- "$resolved_output")"
mv "$staged" "$resolved_output"
