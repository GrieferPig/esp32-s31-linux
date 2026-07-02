#!/bin/bash
# build_opensbi_s31.sh
# Build FW_JUMP for flash XIP at 0x40030000 with RW data in HP SRAM.
# By default this builds a Linux handoff image: OpenSBI executes from flash XIP
# and jumps to the Linux xipImage entry in flash with an S-mode DTB.
set -euo pipefail

OPENSBI_DIR="${HOME}/s31linux/opensbi"
LINUX_DIR="${LINUX_DIR:-${HOME}/s31linux/linux-6.12}"
FDT_SRC="${FDT_SRC:-${LINUX_DIR}/arch/riscv/boot/dts/espressif/esp32s31_smode_generic.dts}"
TARGET_DIR="$(cd "$(dirname "$0")" && pwd)"
CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"
DTC="${DTC:-dtc}"
CPP="${CPP:-${CROSS_COMPILE}cpp}"
FW_TEXT_START="${FW_TEXT_START:-0x40030000}"
FW_RW_START="${FW_RW_START:-0x2F040000}"
LINUX_XIP_ADDR="${LINUX_XIP_ADDR:-0x400B0000}"
OPENSBI_PARTITION_SIZE="${OPENSBI_PARTITION_SIZE:-0x80000}"
OPENSBI_PARTITION_SIZE=$((OPENSBI_PARTITION_SIZE))
OPENSBI_BRINGUP_ONLY="${OPENSBI_BRINGUP_ONLY:-0}"
BRINGUP_HOLD_ADDR="${BRINGUP_HOLD_ADDR:-0x2F07F000}"
OPENSBI_FLASH_OFFSET="${OPENSBI_FLASH_OFFSET:-0x220000}"
if [ "${OPENSBI_BRINGUP_ONLY}" = "1" ] && [ -z "${FW_JUMP_ADDR:-}" ]; then
    FW_JUMP_ADDR="${BRINGUP_HOLD_ADDR}"
else
    FW_JUMP_ADDR="${FW_JUMP_ADDR:-${LINUX_XIP_ADDR}}"
fi

FDT_DTB="${TARGET_DIR}/esp32s31_generic.dtb"
OPENSBI_FW_JUMP_BIN="${OPENSBI_DIR}/build/platform/generic/firmware/fw_jump.bin"
STAGED_FW_JUMP_BIN="$(mktemp)"
FINAL_BIN="${TARGET_DIR}/fw_payload.bin"
OFFSET_BIN="$(mktemp)"

write_le32()
{
    local value="$1"
    printf '%b' "\\$(printf '%03o' $(( value        & 0xff )))\\$(printf '%03o' $(( (value >> 8)  & 0xff )))\\$(printf '%03o' $(( (value >> 16) & 0xff )))\\$(printf '%03o' $(( (value >> 24) & 0xff )))"
}

cd "${OPENSBI_DIR}"
make distclean 2>/dev/null || true

# 1. DTB
echo "--- DTB ---"
mkdir -p "$(dirname "${FDT_DTB}")"
${CPP} -x assembler-with-cpp -nostdinc -undef -D__DTS__ \
    -I "$(dirname "${FDT_SRC}")" \
    -I "${LINUX_DIR}/include" \
    -I "${LINUX_DIR}/arch/riscv/boot/dts" \
    "${FDT_SRC}" | ${DTC} -O dtb -i "$(dirname "${FDT_SRC}")" -o "${FDT_DTB}"

# 2. FW_JUMP
echo "--- FW_JUMP ---"
make \
  CROSS_COMPILE="${CROSS_COMPILE}" \
  PLATFORM=generic \
  PLATFORM_RISCV_XLEN=32 \
  PLATFORM_RISCV_ISA=rv32imac_zicsr_zifencei \
  FW_TEXT_START="${FW_TEXT_START}" \
  FW_RW_START="${FW_RW_START}" \
  FW_JUMP=y \
  FW_JUMP_FDT_OFFSET= \
  FW_JUMP_ADDR="${FW_JUMP_ADDR}" \
  -j"${JOBS}" \
  "$@"

# 3. Append DTB immediately after OpenSBI, pad to 512KB - 4,
#    and store the DTB file offset in the last 4 bytes.  The file to flash is
#    ${TARGET_DIR}/fw_payload.bin, not OpenSBI's own build/.../fw_payload.bin.
cp "${OPENSBI_FW_JUMP_BIN}" "${STAGED_FW_JUMP_BIN}"
FDT_OFFSET="$(stat -c%s "${STAGED_FW_JUMP_BIN}")"
if [ $((FDT_OFFSET % 4)) -ne 0 ]; then
    echo "ERROR: FDT offset ${FDT_OFFSET} is not 4-byte aligned"
    exit 1
fi

cat "${STAGED_FW_JUMP_BIN}" "${FDT_DTB}" > "${FINAL_BIN}"
PAYLOAD_SIZE="$(stat -c%s "${FINAL_BIN}")"
MAX_PAYLOAD_SIZE=$((OPENSBI_PARTITION_SIZE - 4))

if [ "${PAYLOAD_SIZE}" -gt "${MAX_PAYLOAD_SIZE}" ]; then
    echo "ERROR: OpenSBI + FDT (${PAYLOAD_SIZE} bytes) exceeds partition payload limit (${MAX_PAYLOAD_SIZE} bytes)"
    exit 1
fi

write_le32 "${FDT_OFFSET}" > "${OFFSET_BIN}"
truncate -s "${MAX_PAYLOAD_SIZE}" "${FINAL_BIN}"
cat "${OFFSET_BIN}" >> "${FINAL_BIN}"

FINAL_SIZE="$(stat -c%s "${FINAL_BIN}")"
if [ "${FINAL_SIZE}" -ne "${OPENSBI_PARTITION_SIZE}" ]; then
    echo "ERROR: final image size ${FINAL_SIZE} != OpenSBI partition size ${OPENSBI_PARTITION_SIZE}"
    exit 1
fi

if [ "${OPENSBI_BRINGUP_ONLY}" = "1" ]; then
    # Flash XIP: bring-up hold lives in SRAM, not in the payload binary.
    if [ $((FW_TEXT_START)) -ge $((0x40000000)) ] 2>/dev/null; then
        echo "Flash XIP mode: skipping bring-up hold padding (hold at 0x$(printf '%x' ${BRINGUP_HOLD_ADDR}))"
    else
        echo "ERROR: non-XIP bring-up padding is not supported by the appended-FDT partition image"
        exit 1
    fi
fi

echo "=== Result ==="
echo "  Raw FW_JUMP:  ${OPENSBI_FW_JUMP_BIN} ($(stat -c%s "${OPENSBI_FW_JUMP_BIN}") bytes)"
echo "  Flash image:  ${FINAL_BIN}"
echo "  FDT off:  0x$(printf '%x' "${FDT_OFFSET}")"
echo "  Final:    ${FINAL_SIZE} bytes"
echo "  Limit:    ${OPENSBI_PARTITION_SIZE} bytes"
echo "  Text at:  ${FW_TEXT_START}"
echo "  FDT src:  ${FDT_SRC}"
echo "  Next PC:  ${FW_JUMP_ADDR}"
echo "  Bring-up: ${OPENSBI_BRINGUP_ONLY}"
echo "  Flash at:  ${OPENSBI_FLASH_OFFSET} (opensbi partition)"
echo "  Example:   esptool.py write_flash ${OPENSBI_FLASH_OFFSET} ${FINAL_BIN}"
rm -f "${STAGED_FW_JUMP_BIN}" "${OFFSET_BIN}"
