#!/bin/sh
set -eu
: "${BASE_DIR:?Buildroot BASE_DIR missing}"
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${HOST_DIR:?Buildroot HOST_DIR missing}"
shift
exec "${HOST_DIR}/bin/python3" "${script_dir}/scripts/buildroot_finalize.py" --output "$BASE_DIR" "$@"
