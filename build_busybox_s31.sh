#!/bin/bash
# build_busybox_s31.sh - Build the ESP32-S31 BusyBox initramfs payload

set -euo pipefail

TARGET_DIR="$(cd "$(dirname "$0")" && pwd)"
BUSYBOX_DIR="${BUSYBOX_DIR:-${TARGET_DIR}/busybox}"
BUSYBOX_CONFIG="${BUSYBOX_CONFIG:-${TARGET_DIR}/initramfs/busybox_s31.config}"
INIT_SCRIPT="${INIT_SCRIPT:-${TARGET_DIR}/initramfs/init}"
INITRAMFS_ROOT="${INITRAMFS_ROOT:-${TARGET_DIR}/build/s31-initramfs-root}"
INITRAMFS_CPIO="${INITRAMFS_CPIO:-${TARGET_DIR}/build/s31-initramfs.cpio}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-${TARGET_DIR}/initramfs.cpio}"
INITRAMFS_PARTITION_SIZE="${INITRAMFS_PARTITION_SIZE:-0x200000}"
INITRAMFS_PARTITION_SIZE=$((INITRAMFS_PARTITION_SIZE))
JOBS="${JOBS:-$(nproc)}"

echo "=== Building ESP32-S31 BusyBox initramfs ==="
echo "  BusyBox   : ${BUSYBOX_DIR}"
echo "  Config    : ${BUSYBOX_CONFIG}"
echo "  Init      : ${INIT_SCRIPT}"
echo "  Rootfs    : ${INITRAMFS_ROOT}"
echo "  Archive   : ${INITRAMFS_CPIO}"
echo "  Image     : ${INITRAMFS_IMAGE}"
echo "  Jobs      : ${JOBS}"

if [ ! -d "${BUSYBOX_DIR}" ]; then
    echo "ERROR: BusyBox directory not found: ${BUSYBOX_DIR}"
    exit 1
fi

if [ ! -f "${BUSYBOX_CONFIG}" ]; then
    echo "ERROR: BusyBox config fragment not found: ${BUSYBOX_CONFIG}"
    exit 1
fi

if [ ! -f "${INIT_SCRIPT}" ]; then
    echo "ERROR: init script not found: ${INIT_SCRIPT}"
    exit 1
fi

busybox_config_set() {
    local sym="$1"
    local val="$2"
    local cfg="${BUSYBOX_DIR}/.config"
    local tmp="${cfg}.tmp"

    awk -v sym="${sym}" -v val="${val}" '
        BEGIN {
            done = 0
            unset_line = "# " sym " is not set"
        }
        $0 == unset_line {
            print sym "=" val
            done = 1
            next
        }
        index($0, sym "=") == 1 {
            print sym "=" val
            done = 1
            next
        }
        { print }
        END {
            if (!done)
                print sym "=" val
        }
    ' "${cfg}" > "${tmp}"
    mv "${tmp}" "${cfg}"
}

make -C "${BUSYBOX_DIR}" allnoconfig
while IFS= read -r config_line; do
    case "${config_line}" in
        CONFIG_*=*)
            busybox_config_set "${config_line%%=*}" "${config_line#*=}"
            ;;
    esac
done < "${BUSYBOX_CONFIG}"

set +o pipefail
yes "" | make -C "${BUSYBOX_DIR}" oldconfig
oldconfig_status=${PIPESTATUS[1]}
set -o pipefail
if [ "${oldconfig_status}" -ne 0 ]; then
    echo "ERROR: BusyBox oldconfig failed"
    exit "${oldconfig_status}"
fi

make -C "${BUSYBOX_DIR}" -j"${JOBS}"
rm -rf "${INITRAMFS_ROOT}"
make -C "${BUSYBOX_DIR}" CONFIG_PREFIX="${INITRAMFS_ROOT}" install
install -Dm755 "${INIT_SCRIPT}" "${INITRAMFS_ROOT}/init"
mkdir -p "${INITRAMFS_ROOT}/dev" "${INITRAMFS_ROOT}/proc" \
         "${INITRAMFS_ROOT}/sys" "${INITRAMFS_ROOT}/tmp" \
         "${INITRAMFS_ROOT}/run" "${INITRAMFS_ROOT}/etc"
chmod 1777 "${INITRAMFS_ROOT}/tmp"
mkdir -p "$(dirname "${INITRAMFS_CPIO}")"
(
    cd "${INITRAMFS_ROOT}"
    find . -print0 | cpio --null -o -H newc --owner=0:0
) > "${INITRAMFS_CPIO}"

INITRAMFS_SIZE="$(stat -c%s "${INITRAMFS_CPIO}")"
if [ "${INITRAMFS_SIZE}" -gt "${INITRAMFS_PARTITION_SIZE}" ]; then
    echo "ERROR: initramfs ${INITRAMFS_SIZE} bytes exceeds partition size ${INITRAMFS_PARTITION_SIZE}"
    exit 1
fi

cp "${INITRAMFS_CPIO}" "${INITRAMFS_IMAGE}"
truncate -s "${INITRAMFS_PARTITION_SIZE}" "${INITRAMFS_IMAGE}"

echo "=== BusyBox initramfs complete ==="
echo "  Payload size : ${INITRAMFS_SIZE} bytes"
echo "  Flash size   : ${INITRAMFS_PARTITION_SIZE} bytes"
ls -lh "${INITRAMFS_IMAGE}" "${INITRAMFS_CPIO}" "${INITRAMFS_ROOT}/bin/busybox"
