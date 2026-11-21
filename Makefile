# Buildroot owns the kernel, root filesystem and final board image.
.DEFAULT_GOAL := image
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
TARGET ?= esp32s31
OUT_ROOT ?= $(ROOT)/out-buildroot-$(TARGET)
OUTPUT := $(abspath $(OUT_ROOT))
BR2_DL_DIR ?= $(ROOT)/cache/downloads/buildroot
JOBS ?= $(shell nproc)
export P4_CROSS_COMPILE ?= riscv32-esp-elf-

ifeq ($(TARGET),esp32s31)
DEFCONFIG := esp32s31_defconfig
BUILDROOT_SOURCE := $(ROOT)/buildroot
else ifeq ($(TARGET),esp32p4)
DEFCONFIG := esp32p4_rootfs_defconfig
P4_BUILDROOT_PATCH := $(ROOT)/buildroot-external/board/esp32-p4/buildroot-wpa-nommu.patch
BUILDROOT_REV := $(shell git -C "$(ROOT)" rev-parse :buildroot)
P4_PATCH_HASH := $(shell sha256sum "$(P4_BUILDROOT_PATCH)" | cut -d' ' -f1)
BUILDROOT_SOURCE := $(OUTPUT)/.source/buildroot-$(BUILDROOT_REV)-$(P4_PATCH_HASH)
BUILDROOT_PREPARED := $(BUILDROOT_SOURCE)/.prepared

$(BUILDROOT_PREPARED):
	mkdir -p "$(BUILDROOT_SOURCE)"
	git -C "$(ROOT)/buildroot" archive "$(BUILDROOT_REV)" | tar -x -C "$(BUILDROOT_SOURCE)"
	patch -d "$(BUILDROOT_SOURCE)" -p1 < "$(P4_BUILDROOT_PATCH)"
	touch "$@"
else
$(error Unknown TARGET "$(TARGET)"; use esp32s31 or esp32p4)
endif

BUILDROOT = $(MAKE) -C "$(BUILDROOT_SOURCE)" O="$(OUTPUT)" \
	BR2_EXTERNAL="$(ROOT)/buildroot-external" BR2_DL_DIR="$(BR2_DL_DIR)" BR2_JLEVEL="$(JOBS)"

.PHONY: config image build linux rootfs help
config: $(BUILDROOT_PREPARED)
	$(BUILDROOT) $(DEFCONFIG)

$(OUTPUT)/.config: $(ROOT)/buildroot-external/configs/$(DEFCONFIG) $(BUILDROOT_PREPARED)
	$(BUILDROOT) $(DEFCONFIG)

image build rootfs: $(OUTPUT)/.config
	$(BUILDROOT)

linux: $(OUTPUT)/.config
	$(BUILDROOT) linux

help:
	@printf '%s\n' 'make image TARGET=esp32s31 or TARGET=esp32p4' \
		'make linux TARGET=... builds the kernel without assembling the image' \
		'Use OUT_ROOT=... to select an independent Buildroot output directory'
