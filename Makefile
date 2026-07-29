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
ROOTFS_DIR := $(CURDIR)/rootfs
BLUEZ_VERSION := 5.86
BLUEZ_ARCHIVE = $(BUILD_DIR)/bluez-$(BLUEZ_VERSION).tar.xz
BLUEZ_SRC = $(BUILD_DIR)/bluez-$(BLUEZ_VERSION)
BLUEZ_OUT = $(BUILD_DIR)/bluez
BLUEZ_URL := https://mirrors.edge.kernel.org/pub/linux/bluetooth/bluez-$(BLUEZ_VERSION).tar.xz
BLUEZ_SHA256 := 99f144540c6070591e4c53bcb977eb42664c62b7b36cb35a29cf72ded339621d

# Out-of-tree build dirs
OPENSBI_OUT := $(BUILD_DIR)/opensbi
LINUX_OUT := $(BUILD_DIR)/linux
BUSYBOX_OUT := $(BUILD_DIR)/busybox
COREMARK_OUT := $(BUILD_DIR)/coremark
ROOTFS_OUT := $(BUILD_DIR)/rootfs-tools

PARTITIONS_CSV := $(CURDIR)/bootloader/partitions.csv
OPENSBI_OFFSET := $(shell awk -F, '/opensbi/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))
LINUX_OFFSET := $(shell awk -F, '/linux/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))
ROOTFS_OFFSET := $(shell awk -F, '/rootfs/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))

FW_PAYLOAD := $(BUILD_DIR)/fw_payload.bin
XIP_IMAGE := $(BUILD_DIR)/xipImage
ROOTFS_IMG := $(BUILD_DIR)/rootfs.sqfs

IDF_EXPORT := $(shell find $(HOME) -maxdepth 5 -type f -name export.sh 2>/dev/null | grep esp-idf | head -n 1)

.PHONY: all download opensbi linux busybox coremark bluez rootfs initramfs clean fullclean flash-opensbi flash-linux flash-rootfs bootloader flash-bootloader erase

all: download opensbi linux busybox rootfs

$(BUILD_DIR) $(OPENSBI_OUT) $(LINUX_OUT) $(BUSYBOX_OUT) $(COREMARK_OUT) $(ROOTFS_OUT):
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

BUSYBOX_CONFIG := $(ROOTFS_DIR)/busybox_s31.config
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

$(BLUEZ_ARCHIVE): | $(BUILD_DIR)
	wget -c -O $@ $(BLUEZ_URL)
	echo "$(BLUEZ_SHA256)  $@" | sha256sum -c -

$(BLUEZ_SRC)/configure: $(BLUEZ_ARCHIVE)
	tar -xf $< -C $(BUILD_DIR)
	touch $@

$(BLUEZ_OUT)/Makefile: $(BLUEZ_SRC)/configure
	mkdir -p $(BLUEZ_OUT)
	cd $(BLUEZ_OUT) && \
		GLIB_CFLAGS=' ' GLIB_LIBS=' ' DBUS_CFLAGS=' ' DBUS_LIBS=' ' \
		CC="$(CC)" \
		CFLAGS="-Os -march=rv32imac_zicsr_zifencei -mabi=ilp32" \
		$(BLUEZ_SRC)/configure \
			--host=riscv32-unknown-linux-musl \
			--disable-shared --enable-static \
			--with-dbusconfdir=/etc/dbus-1/system.d \
			--with-dbussystembusdir=/usr/share/dbus-1/system-services \
			--with-dbussessionbusdir=/usr/share/dbus-1/services \
			--disable-systemd --disable-udev --disable-cups \
			--disable-manpages --disable-client --disable-monitor \
			--disable-obex --disable-a2dp --disable-avrcp \
			--disable-network --disable-hid --disable-hog \
			--disable-bap --disable-bass --disable-mcp --disable-ccp \
			--disable-vcp --disable-micp --disable-csip \
			--disable-tmap --disable-gmap --disable-asha \
			--disable-hfp --disable-datafiles --enable-deprecated

bluez: $(BLUEZ_OUT)/Makefile
	@echo "--- BlueZ $(BLUEZ_VERSION) ---"
	$(MAKE) -C $(BLUEZ_OUT) -j$(JOBS) tools/hciconfig tools/hcitool
	$(CC) -static -Wl,--gc-sections \
		-o $(BLUEZ_OUT)/tools/hciconfig.static \
		$(BLUEZ_OUT)/tools/hciconfig.o \
		$(BLUEZ_OUT)/lib/.libs/libbluetooth-internal.a
	$(CC) -static -Wl,--gc-sections \
		-o $(BLUEZ_OUT)/tools/hcitool.static \
		$(BLUEZ_OUT)/tools/hcitool.o $(BLUEZ_OUT)/src/oui.o \
		$(BLUEZ_OUT)/lib/.libs/libbluetooth-internal.a
	$(CROSS_COMPILE)strip $(BLUEZ_OUT)/tools/hciconfig.static \
		$(BLUEZ_OUT)/tools/hcitool.static

ROOTFS_ROOT := $(BUILD_DIR)/s31-rootfs
ROOTFS_PARTITION_SIZE ?= 6160384

rootfs: busybox coremark bluez | $(ROOTFS_OUT)
	@echo "--- Rootfs ---"
	rm -rf $(ROOTFS_ROOT)
	$(MAKE) -C $(BUSYBOX_DIR) O=$(BUSYBOX_OUT) CONFIG_PREFIX="$(ROOTFS_ROOT)" install
	$(MAKE) -C $(ROOTFS_DIR) OUT_DIR="$(ROOTFS_OUT)" CROSS_COMPILE="$(CROSS_COMPILE)" DESTDIR="$(ROOTFS_ROOT)" all install
	install -Dm755 $(COREMARK_BIN) $(ROOTFS_ROOT)/sbin/coremark
	install -Dm755 $(BLUEZ_OUT)/tools/hciconfig.static $(ROOTFS_ROOT)/usr/bin/hciconfig
	install -Dm755 $(BLUEZ_OUT)/tools/hcitool.static $(ROOTFS_ROOT)/usr/bin/hcitool
	install -Dm755 $(ROOTFS_DIR)/init $(ROOTFS_ROOT)/init
	install -Dm755 $(ROOTFS_DIR)/default.script $(ROOTFS_ROOT)/usr/share/udhcpc/default.script
	mkdir -p $(ROOTFS_ROOT)/dev $(ROOTFS_ROOT)/proc $(ROOTFS_ROOT)/sys \
		$(ROOTFS_ROOT)/etc $(ROOTFS_ROOT)/home $(ROOTFS_ROOT)/media \
		$(ROOTFS_ROOT)/mnt $(ROOTFS_ROOT)/opt $(ROOTFS_ROOT)/root \
		$(ROOTFS_ROOT)/srv $(ROOTFS_ROOT)/var
	install -m644 $(ROOTFS_DIR)/etc/* $(ROOTFS_ROOT)/etc/
	rm -rf $(ROOTFS_ROOT)/tmp $(ROOTFS_ROOT)/run \
		$(ROOTFS_ROOT)/var/run $(ROOTFS_ROOT)/var/lock \
		$(ROOTFS_ROOT)/var/log $(ROOTFS_ROOT)/var/tmp
	ln -sfn /proc/mounts $(ROOTFS_ROOT)/etc/mtab
	ln -sfn /dev/resolv.conf $(ROOTFS_ROOT)/etc/resolv.conf
	ln -sfn /dev/tmp $(ROOTFS_ROOT)/tmp
	ln -sfn /dev/run $(ROOTFS_ROOT)/run
	ln -sfn /run $(ROOTFS_ROOT)/var/run
	ln -sfn /run/lock $(ROOTFS_ROOT)/var/lock
	ln -sfn /dev/var-log $(ROOTFS_ROOT)/var/log
	ln -sfn /tmp $(ROOTFS_ROOT)/var/tmp
	@rm -f $(ROOTFS_IMG)
	@mksquashfs $(ROOTFS_ROOT) $(ROOTFS_IMG) -comp xz -b 64K -always-use-fragments -all-root
	@ROOTFS_SIZE=$$(stat -c%s $(ROOTFS_IMG)); \
	if [ $$ROOTFS_SIZE -gt $(ROOTFS_PARTITION_SIZE) ]; then echo "ERROR: rootfs too large"; exit 1; fi; \
	truncate -s $(ROOTFS_PARTITION_SIZE) $(ROOTFS_IMG)

# Historical/user-facing name for the root filesystem image.
initramfs: rootfs

clean:
	rm -rf $(BUILD_DIR)

fullclean: clean
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
