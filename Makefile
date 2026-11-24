# Buildroot owns the kernel, root filesystem and final board image.
.DEFAULT_GOAL := image
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
TARGET ?= esp32s31
OUT_ROOT ?= $(ROOT)/out-buildroot-$(TARGET)
OUTPUT := $(abspath $(OUT_ROOT))
BR2_DL_DIR ?= $(ROOT)/cache/downloads/buildroot
JOBS ?= $(shell nproc)
export P4_CROSS_COMPILE ?= riscv32-esp-elf-
BUILDROOT_SOURCE := $(ROOT)/buildroot

ifeq ($(TARGET),esp32s31)
DEFCONFIG := esp32s31_defconfig
else ifeq ($(TARGET),esp32s31-korvo)
DEFCONFIG := esp32s31_defconfig
KORVO_DEFCONFIG := $(OUTPUT)/.korvo_defconfig
else ifeq ($(TARGET),esp32s31-wavesynth)
DEFCONFIG := esp32s31_defconfig
WAVESYNTH_DEFCONFIG := $(OUTPUT)/.wavesynth_defconfig
else ifeq ($(TARGET),esp32p4)
DEFCONFIG := esp32p4_rootfs_defconfig
else
$(error Unknown TARGET "$(TARGET)"; use esp32s31, esp32s31-korvo, esp32s31-wavesynth or esp32p4)
endif

BUILDROOT = $(MAKE) -C "$(BUILDROOT_SOURCE)" O="$(OUTPUT)" \
	BR2_EXTERNAL="$(ROOT)/buildroot-external" BR2_DL_DIR="$(BR2_DL_DIR)" BR2_JLEVEL="$(JOBS)"

.PHONY: config image build linux rootfs help
CONFIG_INPUT := $(ROOT)/buildroot-external/configs/$(DEFCONFIG)
CONFIG_TARGET := $(DEFCONFIG)

ifneq ($(KORVO_DEFCONFIG),)
$(KORVO_DEFCONFIG): $(CONFIG_INPUT) $(ROOT)/Makefile
	mkdir -p "$(OUTPUT)"
	sed -e '/^BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES=/s|"$$| $$(BR2_EXTERNAL_ESP32_S31_PATH)/board/esp32-s31/linux-korvo.config"|' \
		-e '/^BR2_LINUX_KERNEL_INTREE_DTS_NAME=/cBR2_LINUX_KERNEL_INTREE_DTS_NAME="espressif/esp32s31-korvo"' \
		"$<" > "$@"
	printf '%s\n' 'BR2_ROOTFS_POST_IMAGE_SCRIPT_ARGS="--board korvo"' >> "$@"

CONFIG_INPUT := $(KORVO_DEFCONFIG)
CONFIG_TARGET := defconfig BR2_DEFCONFIG="$(KORVO_DEFCONFIG)"
endif

ifneq ($(WAVESYNTH_DEFCONFIG),)
$(WAVESYNTH_DEFCONFIG): $(CONFIG_INPUT) $(ROOT)/Makefile
	mkdir -p "$(OUTPUT)"
	sed -e '/^BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES=/s|"$$| $$(BR2_EXTERNAL_ESP32_S31_PATH)/board/esp32-s31/linux-wavesynth.config"|' \
		-e '/^BR2_LINUX_KERNEL_INTREE_DTS_NAME=/cBR2_LINUX_KERNEL_INTREE_DTS_NAME=""' \
		-e '/^BR2_LINUX_KERNEL_INTREE_DTSO_NAMES=/s|"$$| espressif/esp32s31-overlay-i80-ili9341"|' \
		"$<" > "$@"
	printf '%s\n' 'BR2_PACKAGE_S31_WAVESYNTH=y' 'BR2_PACKAGE_HOST_DTC=y' \
		'BR2_LINUX_KERNEL_DTB_OVERLAY_SUPPORT=y' \
		'BR2_LINUX_KERNEL_CUSTOM_DTS_DIR="$$(BR2_EXTERNAL_ESP32_S31_PATH)/board/esp32-s31/wavesynth-dts"' \
		'BR2_ROOTFS_POST_IMAGE_SCRIPT_ARGS="--board wavesynth"' >> "$@"

CONFIG_INPUT := $(WAVESYNTH_DEFCONFIG)
CONFIG_TARGET := defconfig BR2_DEFCONFIG="$(WAVESYNTH_DEFCONFIG)"
endif

config: $(CONFIG_INPUT)
	$(BUILDROOT) $(CONFIG_TARGET)

$(OUTPUT)/.config: $(CONFIG_INPUT)
	$(BUILDROOT) $(CONFIG_TARGET)

image build: rootfs

rootfs: $(OUTPUT)/.config
	$(BUILDROOT)

ifeq ($(TARGET),esp32s31)
image build:
	$(MAKE) TARGET=esp32s31-korvo OUT_ROOT="$(OUTPUT)-korvo" image
endif

linux: $(OUTPUT)/.config
	$(BUILDROOT) linux

help:
	@printf '%s\n' 'make image TARGET=esp32s31 or TARGET=esp32p4' \
		'make image TARGET=esp32s31-korvo builds only the Korvo image' \
		'make image TARGET=esp32s31-wavesynth builds the CoreBoard i80 synth image' \
		'make linux TARGET=... builds the kernel without assembling the image' \
		'Use OUT_ROOT=... to select an independent Buildroot output directory'
