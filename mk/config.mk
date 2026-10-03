include $(ROOT)/configs/build-versions.mk
-include $(ROOT)/local.mk
# Reject retired selection variables rather than silently selecting a build.
ifneq ($(origin PROFILE),undefined)
$(error PROFILE is removed; every build uses the full board configuration)
endif
ifneq ($(origin S31_LEAN_RADIO),undefined)
$(error S31_LEAN_RADIO is removed; every build uses the full board configuration)
endif
ifneq ($(origin S31_WIFI_ONLY),undefined)
$(error S31_WIFI_ONLY is removed; every build includes Wi-Fi and Bluetooth)
endif
KERNEL_FRAGMENTS := $(ROOT)/configs/kernel/common.config $(ROOT)/configs/kernel/board.config
ifeq ($(DEBUG),1)
KERNEL_FRAGMENTS += $(ROOT)/configs/kernel/debug.config
endif
OUT_ROOT ?= $(ROOT)/out
BUILD_DIR := $(abspath $(OUT_ROOT))
CACHE_DIR ?= $(ROOT)/cache
IMAGES_DIR := $(BUILD_DIR)/images
GENERATED_DIR := $(BUILD_DIR)/generated
STAGING_DIR := $(BUILD_DIR)/staging
REPORTS_DIR := $(BUILD_DIR)/reports
PORT ?= /dev/ttyUSB0
BAUD ?= 2000000
JOBS ?= $(shell nproc)
TOOLCHAIN_DIR ?= $(CACHE_DIR)/toolchains
TOOLCHAIN_PREFIX ?= $(TOOLCHAIN_DIR)/riscv32-esp-linux-musl
CROSSTOOL_NG_DIR ?= $(abspath $(ROOT)/../crosstool-NG)
TOOLCHAIN_RELEASE_ASSET := riscv32-esp-linux-musl.tar.xz
TOOLCHAIN_RELEASE_REPOSITORY ?= GrieferPig/crosstool-NG-s31
TOOLCHAIN_RELEASE_API ?= https://api.github.com/repos/$(TOOLCHAIN_RELEASE_REPOSITORY)/releases/latest
TOOLCHAIN_RELEASE_DOWNLOAD_BASE ?= https://github.com/$(TOOLCHAIN_RELEASE_REPOSITORY)/releases/download
TOOLCHAIN_ARCHIVE := $(CACHE_DIR)/downloads/$(TOOLCHAIN_RELEASE_ASSET)
CROSS_COMPILE := $(TOOLCHAIN_PREFIX)/bin/riscv32-esp-linux-musl-
CC := $(CROSS_COMPILE)gcc
# Kernel remains integer-safe despite the compiler's ILP32F multilib selection.
S31_SAFE_ISA := rv32imabc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs
S31_KERNEL_ISA := rv32imafbc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs
S31_KERNEL_FLAGS := -mabi=ilp32f -mtune=esp-base
OPENSBI_DIR := $(ROOT)/opensbi-esp32-s31
LINUX_DIR := $(ROOT)/linux-esp32-s31
UBOOT_DIR := $(ROOT)/u-boot-esp32-s31
RADIO_IDF_DEPS_DIR := $(ROOT)/firmware/radio/idf_deps
BUILDROOT_DIR := $(ROOT)/buildroot
BUILDROOT_EXTERNAL := $(ROOT)/buildroot-external
OPENSBI_OUT := $(BUILD_DIR)/opensbi
LINUX_OUT := $(BUILD_DIR)/linux
UBOOT_OUT := $(BUILD_DIR)/u-boot
BUILDROOT_OUT := $(BUILD_DIR)/buildroot
RADIO_OUT := $(BUILD_DIR)/radio
LP_OUT := $(BUILD_DIR)/lp
RADIO_IDF_BUILD := $(BUILD_DIR)/idf-radio
BUILDROOT_DL_DIR ?= $(CACHE_DIR)/downloads/buildroot
BTSTACK_SOURCE_DIR ?= $(CACHE_DIR)/sources/btstack
COREMARK_OUT := $(BUILD_DIR)/staging/coremark
COREMARK_BIN := $(COREMARK_OUT)/coremark.exe
S31_LAYOUT_CFG := $(ROOT)/configs/esp32s31-layout.cfg
XIP_IMAGE := $(IMAGES_DIR)/xipImage
UBOOT_ITB := $(IMAGES_DIR)/u-boot.itb
UBOOT_SPL_DTB := $(IMAGES_DIR)/u-boot-spl-dtb.bin
SPL_APP_BIN := $(IMAGES_DIR)/spl_app.bin
ROOTFS_IMG := $(IMAGES_DIR)/rootfs.sqfs
RADIO_IMAGE := $(IMAGES_DIR)/radio.bin
PERSIST_IMG := $(IMAGES_DIR)/persist.jffs2
FDT_DTB := $(IMAGES_DIR)/esp32s31_generic.dtb
IDF_ROOT ?= $(HOME)/.espressif
IDF_EXPORT ?= $(firstword $(wildcard $(IDF_PATH)/export.sh) $(wildcard $(IDF_ROOT)/master/esp-idf/export.sh))
DEFCONFIG ?= esp32s31_defconfig
LINUX_TARGET ?= xipImage
LINUX_CMDLINE ?= earlycon=esp32s31uart,mmio,0x2038a000,115200 console=ttyS0,115200n8 rootfstype=squashfs ro init=/init irqaffinity=0 esp32s31_idle=wfi

HOST_PYTHON ?= $(shell command -v python3)
UBOOT_PYTHONPATH ?=

# Stable timestamps prevent unconditional native version-object rebuilds.
SOURCE_DATE_EPOCH ?= $(shell git -C $(ROOT) log -1 --format=%ct)
export SOURCE_DATE_EPOCH
IDF_TOOLS_PATH ?= $(IDF_ROOT)
export IDF_TOOLS_PATH
