# Makefile for ESP32-S31 Linux

TOOLCHAIN_DIR := $(CURDIR)/toolchain
CROSS_COMPILE := $(TOOLCHAIN_DIR)/riscv32imac-musl/bin/riscv32-unknown-linux-musl-
CC := $(CROSS_COMPILE)gcc
CPP := $(CROSS_COMPILE)cpp
DTC := dtc
JOBS ?= $(shell nproc)

BUILD_DIR := $(CURDIR)/build
OPENSBI_DIR := $(CURDIR)/opensbi-esp32-s31
LINUX_DIR := $(CURDIR)/linux-esp32-s31
BUSYBOX_DIR := $(CURDIR)/busybox
COREMARK_DIR := $(CURDIR)/coremark
INITRAMFS_DIR := $(CURDIR)/initramfs

# Out-of-tree build dirs
OPENSBI_OUT := $(BUILD_DIR)/opensbi
LINUX_OUT := $(BUILD_DIR)/linux
BUSYBOX_OUT := $(BUILD_DIR)/busybox
COREMARK_OUT := $(BUILD_DIR)/coremark
INITRAMFS_OUT := $(BUILD_DIR)/initramfs-tools

PARTITIONS_CSV := $(CURDIR)/bootloader/partitions.csv
OPENSBI_OFFSET := $(shell awk -F, '/opensbi/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))
LINUX_OFFSET := $(shell awk -F, '/linux/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))
INITRAMFS_OFFSET := $(shell awk -F, '/initramfs/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))

FW_PAYLOAD := $(BUILD_DIR)/fw_payload.bin
XIP_IMAGE := $(BUILD_DIR)/xipImage
INITRAMFS_CPIO := $(BUILD_DIR)/initramfs.cpio

IDF_EXPORT := $(shell find $(HOME) -maxdepth 5 -type f -name export.sh 2>/dev/null | grep esp-idf | head -n 1)

.PHONY: all download opensbi linux busybox coremark initramfs clean fullclean flash-opensbi flash-linux flash-initramfs bootloader flash-bootloader erase

all: download opensbi linux busybox initramfs

$(BUILD_DIR) $(OPENSBI_OUT) $(LINUX_OUT) $(BUSYBOX_OUT) $(COREMARK_OUT) $(INITRAMFS_OUT):
	mkdir -p $@

download:
	@echo "--- Download ---"
	git submodule update --init --recursive
	@if [ ! -d "$(TOOLCHAIN_DIR)/riscv32imac-musl" ]; then \
		echo "Downloading toolchain..."; \
		mkdir -p $(TOOLCHAIN_DIR); \
		wget -c -O $(TOOLCHAIN_DIR)/toolchain.tar.gz https://github.com/GrieferPig/esp32-s31-linux/releases/download/toolchain/riscv32imac-musl.tar.gz; \
		tar -xzf $(TOOLCHAIN_DIR)/toolchain.tar.gz -C $(TOOLCHAIN_DIR); \
		rm $(TOOLCHAIN_DIR)/toolchain.tar.gz; \
	fi

FW_TEXT_START ?= 0x40030000
FW_RW_START ?= 0x50F00000
LINUX_XIP_ADDR ?= 0x400B0000
FW_JUMP_ADDR ?= $(LINUX_XIP_ADDR)
OPENSBI_PARTITION_SIZE ?= 524288

FDT_SRC := $(LINUX_DIR)/arch/riscv/boot/dts/espressif/esp32s31_generic.dts
FDT_DTB := $(BUILD_DIR)/esp32s31_generic.dtb
OPENSBI_FW_JUMP_BIN := $(OPENSBI_OUT)/platform/generic/firmware/fw_jump.bin

opensbi: | $(OPENSBI_OUT)
	@echo "--- OpenSBI ---"
	$(CPP) -x assembler-with-cpp -nostdinc -undef -D__DTS__ \
		-I $(dir $(FDT_SRC)) \
		-I $(LINUX_DIR)/include \
		-I $(LINUX_DIR)/arch/riscv/boot/dts \
		$(FDT_SRC) | $(DTC) -O dtb -i $(dir $(FDT_SRC)) -o $(FDT_DTB)
	$(MAKE) -C $(OPENSBI_DIR) O=$(OPENSBI_OUT) \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		PLATFORM=generic \
		PLATFORM_RISCV_XLEN=32 \
		PLATFORM_RISCV_ISA=rv32imac_zicsr_zifencei \
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

linux: | $(LINUX_OUT)
	@echo "--- Linux ---"
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" $(DEFCONFIG)
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" -j$(JOBS) $(LINUX_TARGET) dtbs
	cp -v $(LINUX_OUT)/arch/riscv/boot/$(LINUX_TARGET) $(XIP_IMAGE)
	cp -v $(LINUX_OUT)/arch/riscv/boot/dts/espressif/esp32s31_generic.dtb $(BUILD_DIR)/esp32s31_generic.dtb

BUSYBOX_CONFIG := $(INITRAMFS_DIR)/busybox_s31.config
BUSYBOX_DOT_CONFIG := $(BUSYBOX_OUT)/.config

busybox: | $(BUSYBOX_OUT)
	@echo "--- BusyBox ---"
	cp $(BUSYBOX_CONFIG) $(BUSYBOX_DOT_CONFIG)
	@yes "" | $(MAKE) -C $(BUSYBOX_DIR) O=$(BUSYBOX_OUT) oldconfig >/dev/null
	$(MAKE) -C $(BUSYBOX_DIR) O=$(BUSYBOX_OUT) -j$(JOBS)

COREMARK_BIN := $(COREMARK_OUT)/coremark.exe

coremark: | $(COREMARK_OUT)
	@echo "--- CoreMark ---"
	$(MAKE) -C $(COREMARK_DIR) PORT_DIR=linux OPATH="$(COREMARK_OUT)/" \
		CC="$(CC)" NO_LIBRT=1 ITERATIONS=0 REBUILD=1 \
		XCFLAGS="-static -march=rv32imac_zicsr_zifencei -mabi=ilp32" compile

INITRAMFS_ROOT := $(BUILD_DIR)/s31-initramfs-root
INITRAMFS_PARTITION_SIZE ?= 6160384

initramfs: busybox coremark | $(INITRAMFS_OUT)
	@echo "--- Initramfs ---"
	rm -rf $(INITRAMFS_ROOT)
	$(MAKE) -C $(BUSYBOX_DIR) O=$(BUSYBOX_OUT) CONFIG_PREFIX="$(INITRAMFS_ROOT)" install
	$(MAKE) -C $(INITRAMFS_DIR) OUT_DIR="$(INITRAMFS_OUT)" CROSS_COMPILE="$(CROSS_COMPILE)" DESTDIR="$(INITRAMFS_ROOT)" all install
	install -Dm755 $(COREMARK_BIN) $(INITRAMFS_ROOT)/sbin/coremark
	install -Dm755 $(INITRAMFS_DIR)/init $(INITRAMFS_ROOT)/init
	install -Dm755 $(INITRAMFS_DIR)/default.script $(INITRAMFS_ROOT)/usr/share/udhcpc/default.script
	mkdir -p $(INITRAMFS_ROOT)/dev $(INITRAMFS_ROOT)/proc $(INITRAMFS_ROOT)/sys $(INITRAMFS_ROOT)/tmp $(INITRAMFS_ROOT)/run $(INITRAMFS_ROOT)/etc
	chmod 1777 $(INITRAMFS_ROOT)/tmp
	@cd $(INITRAMFS_ROOT) && find . -print0 | cpio --null -o -H newc --owner=0:0 > $(INITRAMFS_CPIO)
	@INITRAMFS_SIZE=$$(stat -c%s $(INITRAMFS_CPIO)); \
	if [ $$INITRAMFS_SIZE -gt $(INITRAMFS_PARTITION_SIZE) ]; then echo "ERROR: initramfs too large"; exit 1; fi; \
	truncate -s $(INITRAMFS_PARTITION_SIZE) $(INITRAMFS_CPIO)

clean:
	rm -rf $(BUILD_DIR)

fullclean: clean
	rm -rf $(TOOLCHAIN_DIR)

flash-opensbi:
	esptool -p /dev/ttyUSB0 -b 2000000 write-flash $(OPENSBI_OFFSET) $(FW_PAYLOAD)

flash-linux:
	esptool -p /dev/ttyUSB0 -b 2000000 write-flash $(LINUX_OFFSET) $(XIP_IMAGE)

flash-initramfs:
	esptool -p /dev/ttyUSB0 -b 2000000 write-flash $(INITRAMFS_OFFSET) $(INITRAMFS_CPIO)

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

flash-all: flash-opensbi flash-linux flash-initramfs flash-bootloader

erase:
	esptool -p /dev/ttyUSB0 -b 2000000 erase-flash
