#!/bin/bash
# build_busybox_s31.sh - Build the ESP32-S31 BusyBox initramfs payload

set -euo pipefail

TARGET_DIR="$(cd "$(dirname "$0")" && pwd)"
BUSYBOX_DIR="${BUSYBOX_DIR:-${TARGET_DIR}/busybox}"
BUSYBOX_CONFIG="${BUSYBOX_CONFIG:-${TARGET_DIR}/initramfs/busybox_s31.config}"
BUSYBOX_CONFIG_MODE="${BUSYBOX_CONFIG_MODE:-allyesconfig}"
INIT_SCRIPT="${INIT_SCRIPT:-${TARGET_DIR}/initramfs/init}"
INITRAMFS_ROOT="${INITRAMFS_ROOT:-${TARGET_DIR}/build/s31-initramfs-root}"
INITRAMFS_CPIO="${INITRAMFS_CPIO:-${TARGET_DIR}/build/s31-initramfs.cpio}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-${TARGET_DIR}/initramfs.cpio}"
INITRAMFS_PARTITION_SIZE="${INITRAMFS_PARTITION_SIZE:-0x5E0000}"
INITRAMFS_PARTITION_SIZE=$((INITRAMFS_PARTITION_SIZE))
COREMARK_BUILD_SCRIPT="${COREMARK_BUILD_SCRIPT:-${TARGET_DIR}/build_coremark_s31.sh}"
COREMARK_OUT_BIN="${COREMARK_OUT_BIN:-${TARGET_DIR}/build/coremark/coremark}"
SEGFAULT_SRC="${SEGFAULT_SRC:-${TARGET_DIR}/initramfs/segfault.c}"
SEGFAULT_TOOLCHAIN_PREFIX="${SEGFAULT_TOOLCHAIN_PREFIX:-/opt/riscv32imac-musl/bin/riscv32-unknown-linux-musl-}"
SEGFAULT_CC="${SEGFAULT_CC:-${SEGFAULT_TOOLCHAIN_PREFIX}gcc}"
SEGFAULT_STRIP="${SEGFAULT_STRIP:-${SEGFAULT_TOOLCHAIN_PREFIX}strip}"
SEGFAULT_OUT_DIR="${SEGFAULT_OUT_DIR:-${TARGET_DIR}/build/initramfs-tools}"
SEGFAULT_OUT_BIN="${SEGFAULT_OUT_BIN:-${SEGFAULT_OUT_DIR}/segfault}"
SEGFAULT_CFLAGS="${SEGFAULT_CFLAGS:--march=rv32imac_zicsr_zifencei -mabi=ilp32 -static -O2}"
FORKTEST_SRC="${FORKTEST_SRC:-${TARGET_DIR}/initramfs/forktest.c}"
FORKTEST_CC="${FORKTEST_CC:-${SEGFAULT_CC}}"
FORKTEST_STRIP="${FORKTEST_STRIP:-${SEGFAULT_STRIP}}"
FORKTEST_OUT_DIR="${FORKTEST_OUT_DIR:-${SEGFAULT_OUT_DIR}}"
FORKTEST_OUT_BIN="${FORKTEST_OUT_BIN:-${FORKTEST_OUT_DIR}/forktest}"
FORKTEST_CFLAGS="${FORKTEST_CFLAGS:-${SEGFAULT_CFLAGS}}"
JOBS="${JOBS:-$(nproc)}"

busybox_config_drop() {
    local config_file="$1"
    local symbol="$2"
    sed -i -E "/^${symbol}=.*/d;/^# ${symbol} is not set$/d" "${config_file}"
}

busybox_config_set() {
    local config_file="$1"
    local symbol="$2"
    local value="$3"
    busybox_config_drop "${config_file}" "${symbol}"
    printf '%s=%s\n' "${symbol}" "${value}" >> "${config_file}"
}

busybox_config_unset() {
    local config_file="$1"
    local symbol="$2"
    busybox_config_drop "${config_file}" "${symbol}"
    printf '# %s is not set\n' "${symbol}" >> "${config_file}"
}

echo "=== Building ESP32-S31 BusyBox initramfs ==="
echo "  BusyBox   : ${BUSYBOX_DIR}"
echo "  Config    : ${BUSYBOX_CONFIG}"
echo "  ConfigMode: ${BUSYBOX_CONFIG_MODE}"
echo "  Init      : ${INIT_SCRIPT}"
echo "  Rootfs    : ${INITRAMFS_ROOT}"
echo "  Archive   : ${INITRAMFS_CPIO}"
echo "  Image     : ${INITRAMFS_IMAGE}"
echo "  CoreMark  : ${COREMARK_OUT_BIN}"
echo "  Segfault  : ${SEGFAULT_OUT_BIN}"
echo "  Forktest  : ${FORKTEST_OUT_BIN}"
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

if [ ! -x "${COREMARK_BUILD_SCRIPT}" ]; then
    echo "ERROR: CoreMark build script not found or not executable: ${COREMARK_BUILD_SCRIPT}"
    exit 1
fi

if [ ! -f "${SEGFAULT_SRC}" ]; then
    echo "ERROR: segfault source not found: ${SEGFAULT_SRC}"
    exit 1
fi

if [ ! -x "${SEGFAULT_CC}" ]; then
    echo "ERROR: segfault compiler not found: ${SEGFAULT_CC}"
    exit 1
fi

if [ ! -f "${FORKTEST_SRC}" ]; then
    echo "ERROR: forktest source not found: ${FORKTEST_SRC}"
    exit 1
fi

if [ ! -x "${FORKTEST_CC}" ]; then
    echo "ERROR: forktest compiler not found: ${FORKTEST_CC}"
    exit 1
fi

make -C "${BUSYBOX_DIR}" distclean >/dev/null 2>&1 || true
make -C "${BUSYBOX_DIR}" "${BUSYBOX_CONFIG_MODE}"

BUSYBOX_DOT_CONFIG="${BUSYBOX_DIR}/.config"

while IFS='=' read -r raw_symbol raw_value; do
    [ -n "${raw_symbol}" ] || continue
    case "${raw_symbol}" in
        \#\ CONFIG_*)
            symbol="${raw_symbol#\# }"
            symbol="${symbol% is not set}"
            busybox_config_unset "${BUSYBOX_DOT_CONFIG}" "${symbol}"
            ;;
        CONFIG_*)
            busybox_config_set "${BUSYBOX_DOT_CONFIG}" "${raw_symbol}" "${raw_value}"
            ;;
        *)
            ;;
    esac
done < "${BUSYBOX_CONFIG}"

for symbol in \
    CONFIG_PAM \
    CONFIG_SELINUX \
    CONFIG_FEATURE_TAR_SELINUX \
    CONFIG_SELINUXENABLED \
    CONFIG_FEATURE_MOUNT_NFS \
    CONFIG_FEATURE_INETD_RPC \
    CONFIG_FEATURE_WGET_OPENSSL \
    CONFIG_EXTRA_COMPAT \
    CONFIG_FEATURE_VI_REGEX_SEARCH \
    CONFIG_FEATURE_USE_BSS_TAIL \
    CONFIG_HWCLOCK \
    CONFIG_TC \
    CONFIG_FEATURE_TC_INGRESS \
    CONFIG_DEBUG \
    CONFIG_DEBUG_PESSIMIZE \
    CONFIG_DEBUG_SANITIZE \
    CONFIG_UNIT_TEST \
    CONFIG_WERROR \
    CONFIG_NOMMU
do
    busybox_config_unset "${BUSYBOX_DOT_CONFIG}" "${symbol}"
done

for symbol in \
    CONFIG_STATIC \
    CONFIG_LFS \
    CONFIG_BUSYBOX \
    CONFIG_FEATURE_INSTALLER \
    CONFIG_INSTALL_NO_USR \
    CONFIG_INSTALL_APPLET_SYMLINKS \
    CONFIG_CTTYHACK \
    CONFIG_ASH \
    CONFIG_ASH_INTERNAL_GLOB \
    CONFIG_ASH_JOB_CONTROL \
    CONFIG_ASH_ALIAS \
    CONFIG_ASH_ECHO \
    CONFIG_ASH_PRINTF \
    CONFIG_ASH_TEST \
    CONFIG_ASH_SLEEP \
    CONFIG_ASH_HELP \
    CONFIG_FEATURE_SH_MATH \
    CONFIG_FEATURE_SH_MATH_64 \
    CONFIG_SH_IS_ASH \
    CONFIG_BASH_IS_NONE
do
    busybox_config_set "${BUSYBOX_DOT_CONFIG}" "${symbol}" "y"
done

for symbol in \
    CONFIG_SH_IS_HUSH \
    CONFIG_SH_IS_NONE \
    CONFIG_BASH_IS_ASH \
    CONFIG_BASH_IS_HUSH
do
    busybox_config_unset "${BUSYBOX_DOT_CONFIG}" "${symbol}"
done

busybox_config_set "${BUSYBOX_DOT_CONFIG}" "CONFIG_CROSS_COMPILER_PREFIX" "\"/opt/riscv32imac-musl/bin/riscv32-unknown-linux-musl-\""
busybox_config_set "${BUSYBOX_DOT_CONFIG}" "CONFIG_SYSROOT" "\"/opt/riscv32imac-musl/sysroot\""
busybox_config_set "${BUSYBOX_DOT_CONFIG}" "CONFIG_EXTRA_CFLAGS" "\"-march=rv32imac -mabi=ilp32\""
busybox_config_set "${BUSYBOX_DOT_CONFIG}" "CONFIG_EXTRA_LDFLAGS" "\"\""
busybox_config_set "${BUSYBOX_DOT_CONFIG}" "CONFIG_EXTRA_LDLIBS" "\"\""
busybox_config_set "${BUSYBOX_DOT_CONFIG}" "CONFIG_PREFIX" "\"./_install\""

set +o pipefail
yes "" | make -C "${BUSYBOX_DIR}" oldconfig >/dev/null
set -o pipefail

make -C "${BUSYBOX_DIR}" -j"${JOBS}"
"${COREMARK_BUILD_SCRIPT}"
mkdir -p "${SEGFAULT_OUT_DIR}"
"${SEGFAULT_CC}" ${SEGFAULT_CFLAGS} "${SEGFAULT_SRC}" -o "${SEGFAULT_OUT_BIN}"
if [ -x "${SEGFAULT_STRIP}" ]; then
    "${SEGFAULT_STRIP}" "${SEGFAULT_OUT_BIN}" || true
fi
"${FORKTEST_CC}" ${FORKTEST_CFLAGS} "${FORKTEST_SRC}" -o "${FORKTEST_OUT_BIN}"
if [ -x "${FORKTEST_STRIP}" ]; then
    "${FORKTEST_STRIP}" "${FORKTEST_OUT_BIN}" || true
fi
rm -rf "${INITRAMFS_ROOT}"
make -C "${BUSYBOX_DIR}" CONFIG_PREFIX="${INITRAMFS_ROOT}" install
install -Dm755 "${INIT_SCRIPT}" "${INITRAMFS_ROOT}/init"
install -Dm755 "${COREMARK_OUT_BIN}" "${INITRAMFS_ROOT}/bin/coremark"
install -Dm755 "${SEGFAULT_OUT_BIN}" "${INITRAMFS_ROOT}/bin/segfault"
install -Dm755 "${FORKTEST_OUT_BIN}" "${INITRAMFS_ROOT}/bin/forktest"
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
