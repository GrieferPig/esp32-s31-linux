# SPDX-License-Identifier: BSD-2-Clause
LINUX_EXTENSIONS += s31-firmware

ifeq ($(BR2_LINUX_KERNEL_EXT_S31_FIRMWARE),y)
# Source extraction precedes this hook; firmware is only a patch dependency.
# No kernel -> final radio -> kernel dependency is introduced.
define S31_FIRMWARE_PREPARE_KERNEL
	mkdir -p $(LINUX_DIR)/s31-firmware
	$(INSTALL) -m 0644 $(S31_FIRMWARE_PAYLOAD)/generated/esp32s31-radio-imports.S $(LINUX_DIR)/s31-firmware/esp32s31-radio-imports.S
	$(INSTALL) -m 0644 $(S31_FIRMWARE_PAYLOAD)/generated/radio-kernel-symbols.txt $(LINUX_DIR)/s31-firmware/radio-kernel-symbols.txt
endef

# Preserve the proven kernel float ABI. Userspace stays ILP32 in the defconfig.
LINUX_CFLAGS += -march=rv32imafbc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs -mabi=ilp32f -mtune=esp-base
LINUX_MAKE_FLAGS += S31_RADIO_IMPORTS=$(LINUX_DIR)/s31-firmware/esp32s31-radio-imports.S LOCALVERSION=
# LZ4 explicitly selects -O3 upstream; retain the measured flash-size policy.
S31_SIZE_OBJECTS = skbuff gro tcp tcp_input util rx aead_api wpa tx status sta_info esp32s31-radio-wifi lz4_compress lz4hc_compress lz4_decompress traps dev irq-esp32s31-smp select esp32s31-radio-rtos
LINUX_MAKE_FLAGS += $(foreach obj,$(S31_SIZE_OBJECTS),CFLAGS_$(obj).o=-Os)
# Use native modules_install stripping, including debug builds of userspace.
LINUX_MAKE_FLAGS += INSTALL_MOD_STRIP=--strip-debug

# The upstream modules_install XZ command uses a 1 MiB dictionary. Keep the
# board's 64 KiB decoder budget in the standard module directory.
define S31_COMPRESS_RADIO_MODULE
	module=$(TARGET_DIR)/lib/modules/$$(cat $(LINUX_DIR)/include/config/kernel.release)/kernel/drivers/platform/esp32s31-radio.ko; \
		if [ -f "$$module.xz" ]; then xz -df "$$module.xz"; fi; \
		xz --check=crc32 --lzma2=dict=64KiB -f "$$module"
endef
LINUX_POST_INSTALL_TARGET_HOOKS += S31_COMPRESS_RADIO_MODULE

define S31_INSTALL_DT_OVERLAYS
	$(foreach dtso,$(LINUX_DTSO_NAMES), \
		$(INSTALL) -D -m 0644 $(LINUX_ARCH_PATH)/boot/dts/$(dtso).dtbo \
			$(TARGET_DIR)/usr/lib/s31-overlays/$(notdir $(dtso)).dtbo
	)
endef
LINUX_POST_INSTALL_TARGET_HOOKS += S31_INSTALL_DT_OVERLAYS

export S31_GENIMAGE_SCRIPT = $(TOPDIR)/support/scripts/genimage.sh
export S31_KERNEL_CONFIG = $(LINUX_DIR)/.config
export S31_LINUX_DIR = $(LINUX_DIR)
export S31_FIRMWARE_DIR
export S31_CROSS_COMPILE = $(TARGET_CROSS)
endif
