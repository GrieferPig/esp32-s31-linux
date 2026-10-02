# Makefile for ESP32-S31 Linux

include configs/build-versions.mk

ifneq ($(filter download,$(MAKECMDGOALS)),)
ifneq ($(words $(MAKECMDGOALS)),1)
$(error Run make download separately before build targets)
endif
endif

PORT ?= /dev/ttyUSB0
BAUD ?= 2000000

TOOLCHAIN_DIR := $(CURDIR)/toolchain
CROSSTOOL_NG_DIR ?= $(abspath $(CURDIR)/../crosstool-NG)
TOOLCHAIN_PREFIX := $(TOOLCHAIN_DIR)/riscv32-esp-linux-musl
TOOLCHAIN_RELEASE_ASSET := riscv32-esp-linux-musl.tar.xz
TOOLCHAIN_RELEASE_REPOSITORY ?= GrieferPig/crosstool-NG-s31
TOOLCHAIN_RELEASE_API ?= https://api.github.com/repos/$(TOOLCHAIN_RELEASE_REPOSITORY)/releases/latest
TOOLCHAIN_RELEASE_DOWNLOAD_BASE ?= https://github.com/$(TOOLCHAIN_RELEASE_REPOSITORY)/releases/download
CROSS_COMPILE := $(TOOLCHAIN_PREFIX)/bin/riscv32-esp-linux-musl-
CC := $(CROSS_COMPILE)gcc
JOBS ?= $(shell nproc)
S31_BTSTACK_O2 ?= 0

# Keep ordinary userspace on scalar RISC-V/FPU/Zb code.  Xesploop state is
# preserved for explicit tests, but it cannot be safely left live across all
# S-mode return paths used by arbitrary libraries.  XespV remains per-package
# through libesp-simd, whose initializer pins users to hart 1 before executing
# vector instructions.  Kernel and firmware code must remain integer-safe.
S31_SAFE_ISA := rv32imabc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs
S31_KERNEL_ISA := rv32imafbc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs
S31_KERNEL_FLAGS := -mabi=ilp32f -mtune=esp-base

BUILD_DIR := $(CURDIR)/build
OPENSBI_DIR := $(CURDIR)/opensbi-esp32-s31
LINUX_DIR := $(CURDIR)/linux-esp32-s31
UBOOT_DIR := $(CURDIR)/u-boot-esp32-s31
RADIO_IDF_DEPS_DIR := $(CURDIR)/firmware/radio/idf_deps
BUILDROOT_DIR := $(CURDIR)/buildroot
BUILDROOT_EXTERNAL := $(CURDIR)/buildroot-external

# Out-of-tree build dirs
OPENSBI_OUT := $(BUILD_DIR)/opensbi-uboot-minimal
LINUX_OUT := $(BUILD_DIR)/linux-6.18
UBOOT_OUT := $(BUILD_DIR)/u-boot
BUILDROOT_OUT := $(BUILD_DIR)/buildroot
BUILDROOT_DL_DIR := $(BUILD_DIR)/buildroot-dl
BTSTACK_SOURCE_DIR := $(BUILD_DIR)/btstack-source
COREMARK_OUT := $(BUILD_DIR)/coremark
COREMARK_BIN := $(COREMARK_OUT)/coremark.exe
TOOLCHAIN_ARCHIVE := $(BUILD_DIR)/downloads/$(TOOLCHAIN_RELEASE_ASSET)

S31_LAYOUT_CFG := $(CURDIR)/configs/esp32s31-layout.cfg

XIP_IMAGE := $(BUILD_DIR)/xipImage
UBOOT_ITB := $(BUILD_DIR)/u-boot.itb
UBOOT_SPL_DTB := $(BUILD_DIR)/u-boot-spl-dtb.bin
SPL_APP_BIN := $(BUILD_DIR)/spl_app.bin
ROOTFS_IMG := $(BUILD_DIR)/rootfs.sqfs
RADIO_FS_IMG := $(BUILD_DIR)/radio.bin
PERSIST_IMG := $(BUILD_DIR)/persist.jffs2

IDF_ROOT ?= $(HOME)/.espressif
# An explicit installation wins; never select an arbitrary installed checkout.
IDF_EXPORT ?= $(firstword $(wildcard $(IDF_PATH)/export.sh) $(wildcard $(IDF_ROOT)/master/esp-idf/export.sh))

.PHONY: all download toolchain toolchain-source idf-check opensbi uboot flash-image radio-linux-payload radio-idf-deps radio-module radio-package radio-fs linux coremark rootfs initramfs s31-pie-cases btstack-source btstack-notices \
	buildroot-menuconfig buildroot-clean clean fullclean flash-opensbi flash-linux \
	flash-dtb flash-radio flash-rootfs flash-existing-radio flash-existing-rootfs \
	persist flash-persist bootloader flash-bootloader flash-all erase

all: check-layout toolchain uboot linux rootfs flash-image

$(BUILD_DIR) $(OPENSBI_OUT) $(LINUX_OUT) $(UBOOT_OUT) $(BUILDROOT_OUT) $(COREMARK_OUT):
	mkdir -p $@

# Source initialization is explicit and never runs concurrently with builds.
download:
	@if [ -n "$(filter-out download,$(MAKECMDGOALS))" ]; then echo "Run make download separately before build targets" >&2; exit 2; fi
	@echo "--- Download ---"
	git submodule update --init --recursive

toolchain: | $(BUILD_DIR)
	@set -eu; \
	if [ "$(TOOLCHAIN_RELEASE_TAG)" = local ]; then \
		test -x "$(CC)" || { echo "ERROR: no local toolchain installed" >&2; exit 1; }; \
		echo "Using explicitly selected local toolchain at $(TOOLCHAIN_PREFIX)"; \
		exit 0; \
	fi; \
	if [ -x "$(CC)" ] && [ "$(TOOLCHAIN_RELEASE_TAG)" = latest ]; then \
		echo "Using installed toolchain at $(TOOLCHAIN_PREFIX)"; \
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
	if [ "$$installed_tag" = "$$release_tag" ] && [ -x "$(CC)" ]; then \
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
	python3 $(CURDIR)/tools/build_linux_toolchain.py --ct-ng-dir "$(CROSSTOOL_NG_DIR)" --jobs "$(JOBS)" --force

# The FIT's fixed 0x400 external-data position places OpenSBI at the
# 64-byte-aligned NOR XIP address 0x40000400.  Only writable state lives in
# coherent HP SRAM, leaving the retired loader area available to the radio.
FW_TEXT_START ?= 0x40000400
FW_RW_START ?= 0x2F00F000
OPENSBI_MAX_SIZE ?= 262144

FDT_DTB := $(BUILD_DIR)/esp32s31_generic.dtb
OPENSBI_FW_DYNAMIC_BIN := $(OPENSBI_OUT)/platform/generic/firmware/fw_dynamic.bin
OPENSBI_CONFIG_STAMP := $(OPENSBI_OUT)/.s31-link-config

opensbi: toolchain | $(OPENSBI_OUT)
	@echo "--- OpenSBI ---"
	@set -eu; \
	desired='FW_TEXT_START=$(FW_TEXT_START) FW_RW_START=$(FW_RW_START) ISA=$(S31_SAFE_ISA)'; \
	actual=$$(cat "$(OPENSBI_CONFIG_STAMP)" 2>/dev/null || true); \
	if [ "$$actual" != "$$desired" ]; then \
		echo "OpenSBI link configuration changed; rebuilding its output tree"; \
	$(MAKE) -C $(OPENSBI_DIR) O=$(OPENSBI_OUT) clean; \
	mkdir -p "$(OPENSBI_OUT)"; \
	printf '%s\n' "$$desired" > "$(OPENSBI_CONFIG_STAMP)"; \
	fi
	$(MAKE) -C $(OPENSBI_DIR) O=$(OPENSBI_OUT) \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		PLATFORM=generic \
		PLATFORM_DEFCONFIG=esp32s31_defconfig \
		PLATFORM_RISCV_XLEN=32 \
		PLATFORM_RISCV_ISA=$(S31_SAFE_ISA) \
		FW_TEXT_START=$(FW_TEXT_START) \
		FW_RW_START=$(FW_RW_START) \
		FW_DYNAMIC=y \
		-j$(JOBS)
	@size=$$(stat -c%s $(OPENSBI_FW_DYNAMIC_BIN)); \
	if [ $$size -gt $(OPENSBI_MAX_SIZE) ]; then \
		echo "ERROR: OpenSBI fw_dynamic ($$size bytes) overlaps SPL at 0x2f040000"; exit 1; \
	fi

uboot: idf-check opensbi | $(UBOOT_OUT)
	@echo "--- U-Boot SPL + proper ---"
	# ESP-IDF prepends its private Python environment to PATH.  Binman needs
	# the distro pkg_resources/pyelftools modules installed by the host, so keep
	# the system Python ahead of the IDF environment for the U-Boot build only.
	PATH="/usr/bin:/bin:$$PATH" $(MAKE) -C $(UBOOT_DIR) O=$(UBOOT_OUT) ARCH=riscv \
		CROSS_COMPILE="$(CROSS_COMPILE)" espressif_esp32s31_defconfig
	PATH="/usr/bin:/bin:$$PATH" $(MAKE) -C $(UBOOT_DIR) O=$(UBOOT_OUT) ARCH=riscv \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		OPENSBI=$(OPENSBI_FW_DYNAMIC_BIN) -j$(JOBS)
	cp -v $(UBOOT_OUT)/u-boot.itb $(UBOOT_ITB)
	cp -v $(UBOOT_OUT)/spl/u-boot-spl-dtb.bin $(UBOOT_SPL_DTB)
	@work=$$(mktemp -d "$(BUILD_DIR)/spl-wrap.XXXXXX"); \
	trap 'rm -rf "$$work"' EXIT; \
	$(CROSS_COMPILE)objcopy -I binary -O elf32-littleriscv -B riscv \
		$(UBOOT_SPL_DTB) "$$work/spl1.elf"; \
	$(CROSS_COMPILE)objcopy --change-section-address .data=0x2F040000 \
		--rename-section .data=.text,alloc,load,readonly,code,contents \
		--set-start 0x2F040000 "$$work/spl1.elf" "$$work/spl_wrapped.elf"; \
	bash -c "source $(IDF_EXPORT) >/dev/null && esptool --chip esp32s31 elf2image \
		--flash-mode dio --flash-freq 80m --flash-size 16MB \
		--output $(SPL_APP_BIN) $$work/spl_wrapped.elf"

RADIO_IDF_BUILD := $(RADIO_IDF_DEPS_DIR)/build-radio

idf-check:
	@test -f "$(IDF_EXPORT)" || { echo "ERROR: ESP-IDF export.sh not found under $(IDF_ROOT)" >&2; exit 1; }
	python3 tools/check_build_versions.py --idf "$(dir $(IDF_EXPORT))" $(if $(filter 1,$(S31_ALLOW_UNPINNED)),--allow-unpinned)

radio-idf-deps: idf-check
	@echo "--- Build ESP-IDF radio dependency closure ---"
	bash -c "source $(IDF_EXPORT) && cd $(RADIO_IDF_DEPS_DIR) && \
		rm -f build-radio/sdkconfig build-radio/sdkconfig.old && \
		idf.py -B build-radio \
		-D SDKCONFIG=$(RADIO_IDF_BUILD)/sdkconfig \
		-D 'SDKCONFIG_DEFAULTS=$(RADIO_IDF_DEPS_DIR)/sdkconfig.defaults;$(RADIO_IDF_DEPS_DIR)/sdkconfig.radio.defaults' \
		reconfigure && \
		targets=\$$(comm -12 \
			<(sed -n 's|^\(esp-idf/.*\\.a\)$$|\1|p' ../boot_link.txt | sort -u) \
			<(ninja -C build-radio -t targets all | \
			  sed -n 's|^\(esp-idf/.*\\.a\):.*|\1|p' | sort -u)) && \
		test -n \"\$$targets\" && \
		ninja -C build-radio -j$(JOBS) \$$targets"

DEFCONFIG ?= esp32s31_defconfig
LINUX_TARGET ?= xipImage
# The 16 MiB radio image keeps only the devices needed for Wi-Fi/Bluetooth,
# the console, flash/persist and USB mass-storage swap.  Set this to 0 when
# building an image intended to load the optional peripheral overlays.
S31_LEAN_RADIO ?= 1
# CMDLINE_FORCE replaces, rather than extends, the defconfig command line, so
# retain the rootfs and console arguments here as well.  Both HP harts start by
# default; normal device IRQs remain pinned to hart 0 via irqaffinity=0.
LINUX_CMDLINE ?= earlycon=esp32s31uart,mmio,0x2038a000,115200 console=ttyS0,115200n8 rootfstype=squashfs ro init=/init irqaffinity=0 esp32s31_idle=wfi
LINUX_PARTITION_SIZE := $(shell python3 tools/check_s31_layout.py --size KERNEL)

radio-linux-payload: radio-idf-deps
	$(MAKE) -C $(CURDIR)/firmware/radio IDF_ROOT="$(IDF_ROOT)" IDF_EXPORT="$(IDF_EXPORT)" \
		IDF_DEPS_DIR="$(RADIO_IDF_DEPS_DIR)" \
		S31_WIFI_ONLY=0 linux-kbuild

radio-module: linux
	@test -f "$(LINUX_OUT)/drivers/platform/esp32s31-radio.ko"
	@test -f "$(BUILD_DIR)/esp32s31-radio-fw-v1.o"
	@echo "Radio module: $(LINUX_OUT)/drivers/platform/esp32s31-radio.ko"
	@echo "External payload: $(BUILD_DIR)/esp32s31-radio-fw-v1.o"

radio-package:
	+S31_LINUX_OUT=$(LINUX_OUT) CROSS_COMPILE=$(CROSS_COMPILE) $(CURDIR)/tools/build_radio_bundle.sh

RADIO_FS_PARTITION_SIZE := $(shell python3 tools/check_s31_layout.py --size RADIO)
radio-fs: linux rootfs
	python3 tools/build_s31_radio_xip.py \
		--prefix $(CROSS_COMPILE) --kernel $(LINUX_OUT)/vmlinux \
		--payload firmware/radio/linux_radio.localized.o \
		--imports firmware/radio/linux-radio-linked-imports.txt --output $(RADIO_FS_IMG)


linux: check-layout toolchain radio-linux-payload | $(LINUX_OUT)
	@echo "--- Linux ---"
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" $(DEFCONFIG)
	$(LINUX_DIR)/scripts/config --file $(LINUX_OUT)/.config \
		--disable BUILTIN_DTB \
		--disable LOCALVERSION_AUTO \
		--enable FILE_LOCKING \
		--enable RISCV_ISA_C \
		--disable RISCV_ISA_V \
		--disable RISCV_ISA_V_DEFAULT_ENABLE \
		--enable RISCV_ISA_ZBA \
		--enable RISCV_ISA_ZBB \
		--enable RISCV_ISA_ZBC \
		--enable BT \
		--enable BT_BREDR \
		--enable INPUT \
		--disable INPUT_KEYBOARD \
		--disable INPUT_MOUSE \
		--disable INPUT_MOUSEDEV \
		--enable INPUT_EVDEV \
		--enable INPUT_MISC \
		--enable INPUT_UINPUT \
		--enable SWAP \
		--enable SCSI \
		--enable BLK_DEV_SD \
		--enable USB_STORAGE \
		--disable USB_UAS \
		--disable ZRAM \
		--disable ZRAM_BACKEND_LZO \
		--disable ZRAM_DEF_COMP_LZORLE \
		--disable ZRAM_WRITEBACK \
		--disable ZRAM_MEMORY_TRACKING \
		--disable ZRAM_MULTI_COMP \
		--enable MODULES \
		--enable MODULE_UNLOAD \
		--enable FW_LOADER_COMPRESS \
		--enable FW_LOADER_COMPRESS_XZ \
		--enable SMP \
		--set-val NR_CPUS 2 \
		--enable ESP32S31_COPROC_CONTEXT \
		--disable CSD_LOCK_WAIT_DEBUG \
		--disable CSD_LOCK_WAIT_DEBUG_DEFAULT \
		--enable PREEMPT_VOLUNTARY \
		--disable PREEMPT_NONE \
		--disable PREEMPT \
		--enable EPOLL \
		--enable TIMERFD \
		--enable DMATEST \
		--enable HIGH_RES_TIMERS \
		--enable HZ_100 \
		--disable HZ_250 \
		--disable HZ_300 \
		--disable HZ_1000
	@if [ "$(S31_LEAN_RADIO)" = "1" ]; then \
		$(LINUX_DIR)/scripts/config --file $(LINUX_OUT)/.config \
			--set-val THREAD_SIZE_ORDER 1 \
			--disable IPV6 \
			--disable NET_DEVMEM \
			--disable NET_SELFTESTS \
			--disable ETHTOOL_NETLINK \
			--disable CFG80211_CERTIFICATION_ONUS \
			--disable SCSI_PROC_FS \
			--disable USB_NET_DRIVERS \
			--disable USB_DWC2_DUAL_ROLE \
			--enable USB_DWC2_HOST \
			--disable USB_GADGET \
			--disable USB_CONFIGFS \
			--disable USB_ROLE_SWITCH \
			--disable HID \
			--disable USB_HID \
			--disable ETHERNET \
			--disable STMMAC_ETH \
			--disable MOTORCOMM_PHY \
			--disable PHYLIB \
			--disable FIXED_PHY \
			--disable MDIO_BUS \
			--disable PCS_XPCS \
			--disable PPS \
			--disable PTP_1588_CLOCK \
			--disable CAN \
			--disable I2C \
			--disable SPI \
			--disable MMC \
			--disable SOUND \
			--disable SND \
			--disable IIO \
			--disable HWMON \
			--disable PWM \
			--disable COUNTER \
			--disable WATCHDOG \
			--disable EXT4_FS \
			--disable FAT_FS \
			--disable VFAT_FS \
			--disable NLS_CODEPAGE_437 \
			--disable NLS_ISO8859_1 \
			--disable ESP32S31_AXI_GDMA \
			--disable CRYPTO_DEV_ESP32S31 \
			--disable ESP32S31_SYSTEM_TIMERS \
			--disable ESP32S31_GPTIMER; \
	fi
	$(LINUX_DIR)/scripts/config --file $(LINUX_OUT)/.config \
		--enable BT_ESP32S31 --enable ESP32S31_WIFI
	$(LINUX_DIR)/scripts/config --file $(LINUX_OUT)/.config \
		--enable MAC80211 --enable ESP32S31_WIFI_SOFTMAC \
		--enable CC_OPTIMIZE_FOR_PERFORMANCE --disable CC_OPTIMIZE_FOR_SIZE
	@if [ -n "$(LINUX_CMDLINE)" ]; then \
		$(LINUX_DIR)/scripts/config --file $(LINUX_OUT)/.config \
			--set-str CMDLINE "$(LINUX_CMDLINE)"; \
	fi
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" olddefconfig
	$(LINUX_DIR)/scripts/config --file $(LINUX_OUT)/.config --enable ESP32S31_RADIO_XIP
	# Force the single radio link unit to observe the generated payload ABI.
	rm -f \
		$(LINUX_OUT)/drivers/platform/esp32s31-radio-*.o \
		$(LINUX_OUT)/drivers/platform/.esp32s31-radio-*.cmd \
		$(LINUX_OUT)/drivers/platform/esp32s31-radio.o \
		$(LINUX_OUT)/drivers/platform/esp32s31-radio.ko
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" \
		LOCALVERSION= KCFLAGS="-march=$(S31_KERNEL_ISA) $(S31_KERNEL_FLAGS)" \
		-j$(JOBS) $(LINUX_TARGET) modules dtbs
	cp -v $(LINUX_OUT)/arch/riscv/boot/$(LINUX_TARGET) $(XIP_IMAGE)
	@size=$$(stat -c%s $(XIP_IMAGE)); \
	if [ $$size -gt $(LINUX_PARTITION_SIZE) ]; then \
		echo "ERROR: $(LINUX_TARGET) ($$size bytes) overlaps persist at 0xB30000"; exit 1; \
	fi
	cp -v $(LINUX_OUT)/arch/riscv/boot/dts/espressif/esp32s31_generic.dtb $(FDT_DTB)

coremark: rootfs | $(COREMARK_OUT)
	@mkdir -p "$(COREMARK_OUT)"
	@set -- $(BUILDROOT_OUT)/build/coremark-*/coremark; \
		test -x "$$1"; cp -v "$$1" "$(COREMARK_BIN)"
	@echo "CoreMark: $(COREMARK_BIN)"

# Keep this decimal because POSIX test(1) and truncate(1) do not accept the
# partition table's 0x-prefixed value.
ROOTFS_PARTITION_SIZE := $(shell python3 tools/check_s31_layout.py --size ROOTFS)
PERSIST_PARTITION_SIZE := $(shell python3 tools/check_s31_layout.py --size PERSIST)
BUILDROOT_MAKE = S31_RADIO_MODULE=$(LINUX_OUT)/drivers/platform/esp32s31-radio.ko S31_LEAN_RADIO=$(S31_LEAN_RADIO) \
	$(MAKE) -C $(BUILDROOT_DIR) O=$(BUILDROOT_OUT) \
	BR2_EXTERNAL=$(BUILDROOT_EXTERNAL) BR2_DL_DIR=$(BUILDROOT_DL_DIR) \
	S31_DTBO_DIR=$(LINUX_OUT)/arch/riscv/boot/dts/espressif

s31-pie-cases:
	@$(MAKE) --no-print-directory idf-check
	bash -c "source $(IDF_EXPORT) >/dev/null && $(CURDIR)/rootfs/gen_s31_pie_cases.sh $(CURDIR)/rootfs/s31_pie_cases.inc"

btstack-source:
	tools/fetch_btstack_source.sh $(BTSTACK_SOURCE_DIR)

btstack-notices: btstack-source
	tools/build_btstack_notice_bundle.sh $(BTSTACK_SOURCE_DIR) \
		$(BUILD_DIR)/btstack-s31-notices.tar.xz

.PHONY: lp-firmware
lp-firmware: idf-check
	bash -c 'source "$(IDF_EXPORT)" >/dev/null && $(MAKE) -C "$(CURDIR)/firmware/lp" IDF_PATH="$$IDF_PATH" stage'

rootfs: linux toolchain s31-pie-cases btstack-source lp-firmware | $(BUILDROOT_OUT)
	@echo "--- Buildroot rootfs ---"
	$(BUILDROOT_MAKE) esp32s31_rootfs_defconfig
	$(BUILDROOT_MAKE) toolchain-external-custom-rebuild
	$(BUILDROOT_MAKE) toolchain-external-rebuild
	$(BUILDROOT_MAKE) esp-simd-rebuild
	$(BUILDROOT_MAKE) s31-tools-rebuild
	$(BUILDROOT_MAKE) coremark-rebuild
	# Rebuild the pinned, self-contained direct-HCI BTstack appliance after
	# package patch or configuration changes.
	$(BUILDROOT_MAKE) btstack-s31-dirclean
	$(BUILDROOT_MAKE) btstack-s31
	$(BUILDROOT_MAKE)
	cp -v $(BUILDROOT_OUT)/images/rootfs.squashfs $(ROOTFS_IMG)
	@ROOTFS_SIZE=$$(stat -c%s $(ROOTFS_IMG)); \
	echo "Buildroot rootfs: $$ROOTFS_SIZE / $(ROOTFS_PARTITION_SIZE) bytes ($$(( $(ROOTFS_PARTITION_SIZE) - $$ROOTFS_SIZE )) bytes free)"; \
	if [ $$ROOTFS_SIZE -gt $(ROOTFS_PARTITION_SIZE) ]; then \
		echo "ERROR: Buildroot rootfs ($$ROOTFS_SIZE bytes) exceeds partition ($(ROOTFS_PARTITION_SIZE) bytes)"; \
		exit 1; \
	fi

# Historical/user-facing name for the root filesystem image.
initramfs: linux rootfs

# Generate an empty, NOR-compatible JFFS2 image for the persist partition.
# This is separate from normal firmware updates so user data is not erased.
persist: check-layout | $(BUILD_DIR)
	@command -v mkfs.jffs2 >/dev/null || { echo "ERROR: mkfs.jffs2 is required" >&2; exit 1; }
	@staging=$$(mktemp -d "$(BUILD_DIR)/persist.XXXXXX"); \
	trap 'rmdir "$$staging"' EXIT; \
	mkfs.jffs2 -q -e 0x2000 --pad=$(PERSIST_PARTITION_SIZE) \
		-d "$$staging" -o $(PERSIST_IMG)

buildroot-menuconfig: | $(BUILDROOT_OUT)
	$(BUILDROOT_MAKE) esp32s31_rootfs_defconfig
	$(BUILDROOT_MAKE) menuconfig

buildroot-clean:
	rm -rf $(BUILDROOT_OUT)

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C $(CURDIR)/firmware/radio clean
	rm -rf $(RADIO_IDF_BUILD)

fullclean: clean
	@test ! -e $(TOOLCHAIN_DIR) || chmod -R u+w $(TOOLCHAIN_DIR)
	rm -rf $(TOOLCHAIN_DIR)

flash-image: check-layout uboot linux rootfs radio-fs
	@echo "--- Merge official U-Boot flash layout ---"
	bash -c "source $(IDF_EXPORT) >/dev/null && \
		$(CURDIR)/tools/gen_esp_flash_image.sh $(S31_LAYOUT_CFG) $(BUILD_DIR)"
	$(MAKE) build-manifest
	python3 tools/release_assets.py --prefix "$(BUILD_DIR)" --checksums

# OpenSBI is embedded in U-Boot's FIT at the official 0x100000 slot.
flash-opensbi: uboot
	python3 tools/check_s31_layout.py --image-slot UBOOT_ITB "$(UBOOT_ITB)"
	bash -c "source $(S31_LAYOUT_CFG) && source $(IDF_EXPORT) >/dev/null && \
		esptool -p $(PORT) -b $(BAUD) write-flash \
		\$$SLOT_UBOOT_ITB $(UBOOT_ITB)"

flash-dtb: linux
	python3 tools/check_s31_layout.py --image-slot DTB "$(FDT_DTB)"
	bash -c "source $(S31_LAYOUT_CFG) && source $(IDF_EXPORT) >/dev/null && \
		esptool -p $(PORT) -b $(BAUD) write-flash \$$SLOT_DTB $(FDT_DTB)"

flash-radio: radio-fs
	python3 tools/check_s31_layout.py --image-slot RADIO "$(RADIO_FS_IMG)"
	bash -c "source $(S31_LAYOUT_CFG) && source $(IDF_EXPORT) >/dev/null && \
		esptool -p $(PORT) -b $(BAUD) write-flash \$$SLOT_RADIO $(RADIO_FS_IMG)"

flash-linux: linux
	python3 tools/check_s31_layout.py --image-slot DTB "$(FDT_DTB)"
	python3 tools/check_s31_layout.py --image-slot KERNEL "$(XIP_IMAGE)"
	bash -c "source $(S31_LAYOUT_CFG) && source $(IDF_EXPORT) >/dev/null && \
		esptool -p $(PORT) -b $(BAUD) write-flash \
		\$$SLOT_DTB $(FDT_DTB) \$$SLOT_KERNEL $(XIP_IMAGE)"

flash-rootfs: rootfs
	python3 tools/check_s31_layout.py --image-slot ROOTFS "$(ROOTFS_IMG)"
	bash -c "source $(S31_LAYOUT_CFG) && source $(IDF_EXPORT) >/dev/null && \
		esptool -p $(PORT) -b $(BAUD) write-flash \$$SLOT_ROOTFS $(ROOTFS_IMG)"

# Fast hardware iteration after an image has already passed its build target.
# These targets never rebuild dependencies and fail before touching Flash when
# the requested artifact is missing.
flash-existing-radio:
	python3 tools/check_s31_layout.py --image-slot RADIO "$(RADIO_FS_IMG)"
	@test -s "$(RADIO_FS_IMG)"
	bash -c "source $(S31_LAYOUT_CFG) && source $(IDF_EXPORT) >/dev/null && \
		esptool -p $(PORT) -b $(BAUD) write-flash \$$SLOT_RADIO $(RADIO_FS_IMG)"

flash-existing-rootfs:
	python3 tools/check_s31_layout.py --image-slot ROOTFS "$(ROOTFS_IMG)"
	@test -s "$(ROOTFS_IMG)"
	bash -c "source $(S31_LAYOUT_CFG) && source $(IDF_EXPORT) >/dev/null && \
		esptool -p $(PORT) -b $(BAUD) write-flash \$$SLOT_ROOTFS $(ROOTFS_IMG)"

flash-persist: persist
	python3 tools/check_s31_layout.py --image-slot PERSIST "$(PERSIST_IMG)"
	bash -c "source $(S31_LAYOUT_CFG) && source $(IDF_EXPORT) >/dev/null && \
		esptool -p $(PORT) -b $(BAUD) write-flash \$$SLOT_PERSIST $(PERSIST_IMG)"

bootloader: uboot

flash-bootloader: uboot
	python3 tools/check_s31_layout.py --image-slot SPL "$(SPL_APP_BIN)"
	python3 tools/check_s31_layout.py --image-slot UBOOT_ITB "$(UBOOT_ITB)"
	@echo "--- Flash U-Boot SPL + FIT ---"
	bash -c "source $(S31_LAYOUT_CFG) && source $(IDF_EXPORT) >/dev/null && \
		esptool -p $(PORT) -b $(BAUD) write-flash \
		\$$SLOT_SPL $(SPL_APP_BIN) \$$SLOT_UBOOT_ITB $(UBOOT_ITB)"

flash-all: uboot linux rootfs radio-fs
	python3 tools/check_s31_layout.py --image-slot SPL "$(SPL_APP_BIN)"
	python3 tools/check_s31_layout.py --image-slot UBOOT_ITB "$(UBOOT_ITB)"
	python3 tools/check_s31_layout.py --image-slot DTB "$(FDT_DTB)"
	python3 tools/check_s31_layout.py --image-slot RADIO "$(RADIO_FS_IMG)"
	python3 tools/check_s31_layout.py --image-slot KERNEL "$(XIP_IMAGE)"
	python3 tools/check_s31_layout.py --image-slot ROOTFS "$(ROOTFS_IMG)"
	@echo "--- Flash complete U-Boot/Linux image (persist preserved) ---"
	bash -c "source $(S31_LAYOUT_CFG) && source $(IDF_EXPORT) >/dev/null && \
		esptool -p $(PORT) -b $(BAUD) write-flash \
		\$$SLOT_SPL $(SPL_APP_BIN) \
		\$$SLOT_UBOOT_ITB $(UBOOT_ITB) \
		\$$SLOT_DTB $(FDT_DTB) \
		\$$SLOT_RADIO $(RADIO_FS_IMG) \
		\$$SLOT_KERNEL $(XIP_IMAGE) \
		\$$SLOT_ROOTFS $(ROOTFS_IMG)"

erase:
	bash -c "source $(IDF_EXPORT) >/dev/null && esptool -p $(PORT) -b $(BAUD) erase-flash"

.PHONY: check-layout check-host check-docs check-dt check-fast build-manifest
check-layout:
	python3 tools/check_s31_layout.py
check-host: check-layout btstack-source
	python3 -m unittest discover -s tools/tests -v
check-docs:
	$(MAKE) -C docs html SPHINXOPTS="-n -W --keep-going"
check-dt:
	python3 tools/check_s31_dt.py --cross-compile "$(CROSS_COMPILE)"
check-fast: check-host check-docs check-dt
build-manifest:
	python3 tools/build_manifest.py --compiler "$(CC)" --idf "$(dir $(IDF_EXPORT))" --output "$(BUILD_DIR)/build-manifest.json"
