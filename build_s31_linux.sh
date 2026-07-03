#!/bin/bash
# build_s31_linux.sh — Build ESP32-S31 Linux kernel and copy artifacts
#
# Prerequisites:
#   - ESP32-S31 RISC-V toolchain in PATH (riscv32-esp-elf-*)
#   - Linux source tree at ~/linux (with esp32-s31-linux branch)
#   - ARCH=riscv, CROSS_COMPILE set for rv32
#
# Output:
#   ./xipImage   — Linux kernel image (RISC-V XIP Image format)
#   ./esp32s31_smode_generic.dtb  — Device tree blob (S-mode CLINT)
#   ./initramfs.cpio — padded BusyBox initramfs partition image

set -euo pipefail

LINUX_DIR="${LINUX_DIR:-${HOME}/s31linux/linux-6.12}"
TARGET_DIR="$(cd "$(dirname "$0")" && pwd)"   # current folder (s31linux)
BUSYBOX_DIR="${BUSYBOX_DIR:-${TARGET_DIR}/busybox}"
BUSYBOX_CONFIG="${BUSYBOX_CONFIG:-${TARGET_DIR}/initramfs/busybox_s31.config}"
BUSYBOX_BUILD_SCRIPT="${BUSYBOX_BUILD_SCRIPT:-${TARGET_DIR}/build_busybox_s31.sh}"

ARCH="riscv"
CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}"
DEFCONFIG="${DEFCONFIG:-esp32s31_xip_defconfig}"
# XIP kernel: raw RISC-V xipImage target, flashed to the linux partition.
TARGET="${TARGET:-xipImage}"
INITRAMFS_ROOT="${INITRAMFS_ROOT:-${TARGET_DIR}/build/s31-initramfs-root}"
INITRAMFS_CPIO="${INITRAMFS_CPIO:-${TARGET_DIR}/build/s31-initramfs.cpio}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-${TARGET_DIR}/initramfs.cpio}"
INITRAMFS_PARTITION_SIZE="${INITRAMFS_PARTITION_SIZE:-0x200000}"
INITRAMFS_PARTITION_SIZE=$((INITRAMFS_PARTITION_SIZE))
INIT_SCRIPT="${INIT_SCRIPT:-${TARGET_DIR}/initramfs/init}"

JOBS="${JOBS:-$(nproc)}"

echo "=== Building ESP32-S31 Linux kernel ==="
echo "  Linux dir : ${LINUX_DIR}"
echo "  ARCH      : ${ARCH}"
echo "  Toolchain : ${CROSS_COMPILE}"
echo "  Defconfig : ${DEFCONFIG}"
echo "  Target    : ${TARGET}"
echo "  BusyBox   : ${BUSYBOX_DIR}"
echo "  Initramfs : ${INITRAMFS_IMAGE}"
echo "  Jobs      : ${JOBS}"
echo "  Out dir   : ${TARGET_DIR}"

if [ ! -x "${BUSYBOX_BUILD_SCRIPT}" ]; then
    echo "ERROR: BusyBox build script not found or not executable: ${BUSYBOX_BUILD_SCRIPT}"
    exit 1
fi

echo ""
echo "--- Step 1: build BusyBox initramfs ---"
"${BUSYBOX_BUILD_SCRIPT}"
INITRAMFS_SIZE="$(stat -c%s "${INITRAMFS_CPIO}")"

cd "${LINUX_DIR}"

# Step 2: apply defconfig
echo ""
echo "--- Step 2: make ${DEFCONFIG} ---"
make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" "${DEFCONFIG}"

# Disable EFI — it wraps Image in a PE header which OpenSBI can't boot
echo "--- Disabling EFI ---"
scripts/config --disable CONFIG_EFI
scripts/config --disable CONFIG_EFI_STUB
scripts/config --disable CONFIG_EFI_GENERIC_STUB
scripts/config --disable CONFIG_FPU
scripts/config --enable CONFIG_BLK_DEV_INITRD
scripts/config --set-str CONFIG_INITRAMFS_SOURCE ""
scripts/config --set-str CONFIG_CMDLINE "earlycon=esp32s3uart,mmio32,0x2038a000 earlycon=sbi console=ttyS0,115200 keep_bootcon earlyprintk debug rdinit=/init"
make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" olddefconfig

# Step 3: build kernel + dtbs
echo ""
echo "--- Step 3: make ${TARGET} + dtbs ---"
make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" -j"${JOBS}" "${TARGET}" dtbs

# Step 4: copy artifacts to target directory
echo ""
echo "--- Step 4: copying artifacts ---"

KERNEL_IMAGE="${LINUX_DIR}/arch/riscv/boot/${TARGET}"
DTB_FILE="${LINUX_DIR}/arch/riscv/boot/dts/espressif/esp32s31_smode_generic.dtb"

if [ ! -f "${KERNEL_IMAGE}" ]; then
    echo "ERROR: ${KERNEL_IMAGE} not found"
    exit 1
fi

if [ ! -f "${DTB_FILE}" ]; then
    echo "ERROR: ${DTB_FILE} not found"
    exit 1
fi

cp -v "${KERNEL_IMAGE}" "${TARGET_DIR}/${TARGET}"
cp -v "${DTB_FILE}"   "${TARGET_DIR}/esp32s31_smode_generic.dtb"

echo ""
echo "=== Build complete ==="
echo "  Kernel : ${TARGET_DIR}/${TARGET}"
echo "  DTB    : ${TARGET_DIR}/esp32s31_smode_generic.dtb"
echo "  Initrd : ${INITRAMFS_IMAGE} (${INITRAMFS_SIZE} bytes payload, ${INITRAMFS_PARTITION_SIZE} bytes flashed)"
ls -lh "${TARGET_DIR}/${TARGET}" "${TARGET_DIR}/esp32s31_smode_generic.dtb" "${INITRAMFS_IMAGE}"
