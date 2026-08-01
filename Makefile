# Makefile for ESP32-S31 Linux

TOOLCHAIN_DIR := $(CURDIR)/toolchain
CROSSTOOL_NG_DIR ?= $(abspath $(CURDIR)/../crosstool-NG)
CROSSTOOL_CONFIG := $(CURDIR)/configs/riscv32-esp-linux-musl.config
TOOLCHAIN_PREFIX := $(TOOLCHAIN_DIR)/riscv32-esp-linux-musl
TOOLCHAIN_RELEASE_TAG ?= latest
TOOLCHAIN_RELEASE_ASSET := riscv32-esp-linux-musl.tar.xz
TOOLCHAIN_RELEASE_REPOSITORY ?= GrieferPig/crosstool-NG-s31
TOOLCHAIN_RELEASE_API ?= https://api.github.com/repos/$(TOOLCHAIN_RELEASE_REPOSITORY)/releases/latest
TOOLCHAIN_RELEASE_DOWNLOAD_BASE ?= https://github.com/$(TOOLCHAIN_RELEASE_REPOSITORY)/releases/download
CROSS_COMPILE := $(TOOLCHAIN_PREFIX)/bin/riscv32-esp-linux-musl-
CC := $(CROSS_COMPILE)gcc
CPP := $(CROSS_COMPILE)cpp
DTC := dtc
JOBS ?= $(shell nproc)

# S31 supports F and the stateful Espressif HWLoop/PIE extensions, but firmware
# and kernel C code must not borrow task coprocessor state.  Use every safe
# integer code-generation extension there, and expose the complete ISA to
# userspace where Linux saves/restores that state.
S31_SAFE_ISA := rv32imabc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs
S31_USER_ISA := rv32imafbc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs_xesploop_xespv2p2
S31_COMMON_FLAGS := -mabi=ilp32 -mtune=esp-base
S31_USER_FLAGS := -march=$(S31_USER_ISA) $(S31_COMMON_FLAGS) -mespv-spec=2p2

BUILD_DIR := $(CURDIR)/build
OPENSBI_DIR := $(CURDIR)/opensbi-esp32-s31
LINUX_DIR := $(CURDIR)/linux-esp32-s31
COREMARK_DIR := $(CURDIR)/coremark
COREMARK_ITERATIONS ?= 20000
BUILDROOT_DIR := $(CURDIR)/buildroot
BUILDROOT_EXTERNAL := $(CURDIR)/buildroot-external

# Out-of-tree build dirs
OPENSBI_OUT := $(BUILD_DIR)/opensbi
LINUX_OUT := $(BUILD_DIR)/linux
COREMARK_OUT := $(BUILD_DIR)/coremark
BUILDROOT_OUT := $(BUILD_DIR)/buildroot
BUILDROOT_DL_DIR := $(BUILD_DIR)/buildroot-dl
TOOLCHAIN_ARCHIVE := $(BUILD_DIR)/downloads/$(TOOLCHAIN_RELEASE_ASSET)

PARTITIONS_CSV := $(CURDIR)/bootloader/partitions.csv
OPENSBI_OFFSET := $(shell awk -F, '/opensbi/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))
LINUX_OFFSET := $(shell awk -F, '/linux/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))
ROOTFS_OFFSET := $(shell awk -F, '/rootfs/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))

FW_PAYLOAD := $(BUILD_DIR)/fw_payload.bin
XIP_IMAGE := $(BUILD_DIR)/xipImage
ROOTFS_IMG := $(BUILD_DIR)/rootfs.sqfs

IDF_EXPORT := $(shell find $(HOME) -maxdepth 5 -type f -name export.sh 2>/dev/null | grep esp-idf | head -n 1)

.PHONY: all download toolchain toolchain-source opensbi linux coremark rootfs initramfs s31-pie-cases \
	buildroot-menuconfig buildroot-clean clean fullclean flash-opensbi flash-linux \
	flash-rootfs bootloader flash-bootloader erase

all: toolchain download opensbi linux initramfs

$(BUILD_DIR) $(OPENSBI_OUT) $(LINUX_OUT) $(COREMARK_OUT) $(BUILDROOT_OUT):
	mkdir -p $@

download: toolchain
	@echo "--- Download ---"
	git submodule update --init --recursive

toolchain: | $(BUILD_DIR)
	@set -eu; \
	if [ -x "$(CC)" ] && { [ -f "$(TOOLCHAIN_PREFIX)/.source-build" ] || [ ! -f "$(TOOLCHAIN_PREFIX)/.release" ]; }; then \
		echo "Using locally built toolchain at $(TOOLCHAIN_PREFIX)"; \
		exit 0; \
	fi; \
	mkdir -p "$(dir $(TOOLCHAIN_ARCHIVE))" "$(TOOLCHAIN_DIR)"; \
	release_tag="$(TOOLCHAIN_RELEASE_TAG)"; \
	if [ "$$release_tag" = latest ]; then \
		release_tag=$$(curl --fail --location --retry 3 --silent --show-error "$(TOOLCHAIN_RELEASE_API)" | sed -n 's/^[[:space:]]*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p'); \
	fi; \
	if [ -z "$$release_tag" ]; then \
		echo "ERROR: failed to resolve the latest toolchain release tag" >&2; exit 1; \
	fi; \
	release_url="$(TOOLCHAIN_RELEASE_DOWNLOAD_BASE)/$$release_tag/$(TOOLCHAIN_RELEASE_ASSET)"; \
	release_sha256_url="$$release_url.sha256"; \
	installed_tag=$$(cat "$(TOOLCHAIN_PREFIX)/.release" 2>/dev/null || true); \
	if [ -z "$$installed_tag" ] && [ -d "$(TOOLCHAIN_PREFIX)" ]; then \
		installed_tag=$$(find "$(TOOLCHAIN_PREFIX)" -maxdepth 1 -type f -name '.release-*' -printf '%f\n' 2>/dev/null | sed 's/^\.release-//' | head -n 1); \
	fi; \
	if [ "$$installed_tag" = "$$release_tag" ]; then \
		echo "Toolchain release $$release_tag is already installed"; \
		exit 0; \
	fi; \
	echo "Installing toolchain release $$release_tag"; \
	curl --fail --location --retry 3 --output "$(TOOLCHAIN_ARCHIVE).part" "$$release_url"; \
	curl --fail --location --retry 3 --output "$(TOOLCHAIN_ARCHIVE).sha256.part" "$$release_sha256_url"; \
	expected_hash=$$(awk 'NR == 1 { print $$1; exit }' "$(TOOLCHAIN_ARCHIVE).sha256.part"); \
	printf '%s\n' "$$expected_hash" | grep -Eq '^[0-9a-fA-F]{64}$$' || { echo "ERROR: invalid release checksum" >&2; exit 1; }; \
	printf '%s  %s\n' "$$expected_hash" "$(TOOLCHAIN_ARCHIVE).part" | sha256sum --check -; \
	mv "$(TOOLCHAIN_ARCHIVE).part" "$(TOOLCHAIN_ARCHIVE)"; \
	rm -f "$(TOOLCHAIN_ARCHIVE).sha256.part"; \
	staging=$$(mktemp -d "$(TOOLCHAIN_DIR)/.riscv32-esp-linux-musl.XXXXXX"); \
	trap 'chmod -R u+w "$$staging" 2>/dev/null || true; rm -rf "$$staging"' EXIT; \
	tar -xJf "$(TOOLCHAIN_ARCHIVE)" -C "$$staging"; \
	test -x "$$staging/bin/riscv32-esp-linux-musl-gcc"; \
	printf '%s\n' "$$release_tag" > "$$staging/.release"; \
	printf '%s\n' "$$release_tag" > "$$staging/.release-$$release_tag"; \
	chmod u-w "$$staging"; \
	if [ -e "$(TOOLCHAIN_PREFIX)" ]; then \
		backup="$(TOOLCHAIN_PREFIX).previous.$$(date -u +%Y%m%d%H%M%S)"; \
		mv "$(TOOLCHAIN_PREFIX)" "$$backup"; \
		echo "Previous toolchain retained at $$backup"; \
	fi; \
	mv "$$staging" "$(TOOLCHAIN_PREFIX)"; \
	trap - EXIT; \
	"$(CC)" --version | head -n 1

toolchain-source:
	python3 $(CURDIR)/build_linux_toolchain.py --ct-ng-dir "$(CROSSTOOL_NG_DIR)" --jobs "$(JOBS)" --force

FW_TEXT_START ?= 0x40030000
FW_RW_START ?= 0x50F00000
LINUX_XIP_ADDR ?= 0x400B0000
FW_JUMP_ADDR ?= $(LINUX_XIP_ADDR)
OPENSBI_PARTITION_SIZE ?= 524288

FDT_SRC := $(LINUX_DIR)/arch/riscv/boot/dts/espressif/esp32s31_generic.dts
FDT_DTB := $(BUILD_DIR)/esp32s31_generic.dtb
OPENSBI_FW_JUMP_BIN := $(OPENSBI_OUT)/platform/generic/firmware/fw_jump.bin

opensbi: toolchain | $(OPENSBI_OUT)
	@echo "--- OpenSBI ---"
	$(CPP) -x assembler-with-cpp -nostdinc -undef -D__DTS__ \
		-I $(dir $(FDT_SRC)) \
		-I $(LINUX_DIR)/include \
		-I $(LINUX_DIR)/arch/riscv/boot/dts \
		$(FDT_SRC) | $(DTC) -@ -O dtb -i $(dir $(FDT_SRC)) -o $(FDT_DTB)
	$(MAKE) -C $(OPENSBI_DIR) O=$(OPENSBI_OUT) \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		PLATFORM=generic \
		PLATFORM_RISCV_XLEN=32 \
		PLATFORM_RISCV_ISA=$(S31_SAFE_ISA) \
		FW_TEXT_START=$(FW_TEXT_START) \
		FW_RW_START=$(FW_RW_START) \
		FW_JUMP=y \
		FW_JUMP_FDT_OFFSET= \
		FW_JUMP_ADDR=$(FW_JUMP_ADDR) \
		-j$(JOBS)
	@cp $(OPENSBI_FW_JUMP_BIN) $(BUILD_DIR)/staged_fw_jump.bin
	@truncate -s 262144 $(BUILD_DIR)/staged_fw_jump.bin
	@FDT_OFFSET=262144; \
	cat $(BUILD_DIR)/staged_fw_jump.bin $(FDT_DTB) > $(FW_PAYLOAD); \
	PAYLOAD_SIZE=$$(stat -c%s $(FW_PAYLOAD)); \
	MAX_PAYLOAD_SIZE=$$(( $(OPENSBI_PARTITION_SIZE) - 4 )); \
	if [ $$PAYLOAD_SIZE -gt $$MAX_PAYLOAD_SIZE ]; then echo "ERROR: Payload exceeds limit"; exit 1; fi; \
	python3 -c "import sys, struct; sys.stdout.buffer.write(struct.pack('<I', $$FDT_OFFSET))" > $(BUILD_DIR)/offset.bin; \
	truncate -s $$MAX_PAYLOAD_SIZE $(FW_PAYLOAD); \
	cat $(BUILD_DIR)/offset.bin >> $(FW_PAYLOAD); \
	rm -f $(BUILD_DIR)/staged_fw_jump.bin $(BUILD_DIR)/offset.bin

DEFCONFIG ?= esp32s31_defconfig
LINUX_TARGET ?= xipImage

linux: toolchain | $(LINUX_OUT)
	@echo "--- Linux ---"
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" $(DEFCONFIG)
	$(LINUX_DIR)/scripts/config --file $(LINUX_OUT)/.config \
		--set-str BUILTIN_DTB_SOURCE "espressif/esp32s31_generic" \
		--enable RISCV_ISA_C \
		--disable RISCV_ISA_V \
		--disable RISCV_ISA_V_DEFAULT_ENABLE \
		--enable RISCV_ISA_ZBA \
		--enable RISCV_ISA_ZBB \
		--enable RISCV_ISA_ZBC
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" olddefconfig
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" \
		KCFLAGS="-march=$(S31_SAFE_ISA) $(S31_COMMON_FLAGS)" -j$(JOBS) $(LINUX_TARGET) dtbs
	cp -v $(LINUX_OUT)/arch/riscv/boot/$(LINUX_TARGET) $(XIP_IMAGE)
	cp -v $(LINUX_OUT)/arch/riscv/boot/dts/espressif/esp32s31_generic.dtb $(FDT_DTB)

coremark: toolchain | $(COREMARK_OUT)
	@echo "--- CoreMark ---"
	$(MAKE) -C $(COREMARK_DIR) PORT_DIR=linux OPATH="$(COREMARK_OUT)/" \
		CC="$(CC)" NO_LIBRT=1 ITERATIONS=$(COREMARK_ITERATIONS) REBUILD=1 \
		XCFLAGS="-static $(S31_USER_FLAGS) -include $(BUILDROOT_EXTERNAL)/package/coremark-s31/coremark-monotonic.h" compile

# Keep this decimal because POSIX test(1) and truncate(1) do not accept the
# partition table's 0x-prefixed value.
ROOTFS_PARTITION_SIZE ?= 6144000
BUILDROOT_MAKE = $(MAKE) -C $(BUILDROOT_DIR) O=$(BUILDROOT_OUT) \
	BR2_EXTERNAL=$(BUILDROOT_EXTERNAL) BR2_DL_DIR=$(BUILDROOT_DL_DIR)

s31-pie-cases:
	@if [ -z "$(IDF_EXPORT)" ]; then echo "ERROR: ESP-IDF export.sh not found under $(HOME)"; exit 1; fi
	bash -c "source $(IDF_EXPORT) >/dev/null && $(CURDIR)/rootfs/gen_s31_pie_cases.sh $(CURDIR)/rootfs/s31_pie_cases.inc"

rootfs: toolchain s31-pie-cases | $(BUILDROOT_OUT)
	@echo "--- Buildroot rootfs ---"
	$(BUILDROOT_MAKE) esp32s31_rootfs_defconfig
	$(BUILDROOT_MAKE) toolchain-external-custom-rebuild
	$(BUILDROOT_MAKE) toolchain-external-rebuild
	$(BUILDROOT_MAKE) s31-tools-rebuild
	$(BUILDROOT_MAKE)
	cp -v $(BUILDROOT_OUT)/images/rootfs.squashfs $(ROOTFS_IMG)
	@ROOTFS_SIZE=$$(stat -c%s $(ROOTFS_IMG)); \
	if [ $$ROOTFS_SIZE -gt $(ROOTFS_PARTITION_SIZE) ]; then \
		echo "ERROR: Buildroot rootfs ($$ROOTFS_SIZE bytes) exceeds partition ($(ROOTFS_PARTITION_SIZE) bytes)"; \
		exit 1; \
	fi; \
	truncate -s $(ROOTFS_PARTITION_SIZE) $(ROOTFS_IMG)

# Historical/user-facing name for the root filesystem image.
initramfs: linux rootfs

buildroot-menuconfig: | $(BUILDROOT_OUT)
	$(BUILDROOT_MAKE) esp32s31_rootfs_defconfig
	$(BUILDROOT_MAKE) menuconfig

buildroot-clean:
	rm -rf $(BUILDROOT_OUT)

clean:
	rm -rf $(BUILD_DIR)

fullclean: clean
	@test ! -e $(TOOLCHAIN_DIR) || chmod -R u+w $(TOOLCHAIN_DIR)
	rm -rf $(TOOLCHAIN_DIR)

flash-opensbi:
	esptool -p /dev/ttyUSB0 -b 2000000 write-flash $(OPENSBI_OFFSET) $(FW_PAYLOAD)

flash-linux:
	esptool -p /dev/ttyUSB0 -b 2000000 write-flash $(LINUX_OFFSET) $(XIP_IMAGE)

flash-rootfs:
	esptool -p /dev/ttyUSB0 -b 2000000 write-flash $(ROOTFS_OFFSET) $(ROOTFS_IMG)

bootloader:
	@if [ -z "$(IDF_EXPORT)" ]; then echo "ERROR: ESP-IDF export.sh not found under $(HOME)"; exit 1; fi
	@echo "--- Build Bootloader ---"
	@echo "Using ESP-IDF from $(IDF_EXPORT)"
	bash -c "source $(IDF_EXPORT) && cd $(CURDIR)/bootloader && idf.py build"

flash-bootloader:
	@if [ -z "$(IDF_EXPORT)" ]; then echo "ERROR: ESP-IDF export.sh not found under $(HOME)"; exit 1; fi
	@echo "--- Flash Bootloader ---"
	@echo "Using ESP-IDF from $(IDF_EXPORT)"
	bash -c "source $(IDF_EXPORT) && cd $(CURDIR)/bootloader && idf.py flash -p /dev/ttyUSB0 -b 2000000"

flash-all: flash-opensbi flash-linux flash-rootfs flash-bootloader

erase:
	esptool -p /dev/ttyUSB0 -b 2000000 erase-flash
