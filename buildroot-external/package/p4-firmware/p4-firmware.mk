# SPDX-License-Identifier: BSD-2-Clause
P4_FIRMWARE_VERSION = fw-v0.3.2
P4_FIRMWARE_SITE = https://github.com/GrieferPig/esp32-s31-linux-firmware/releases/download/$(P4_FIRMWARE_VERSION)
P4_FIRMWARE_SOURCE = p4-firmware-inputs.tar
P4_FIRMWARE_STRIP_COMPONENTS = 0
P4_FIRMWARE_LICENSE = GPL-2.0+
P4_FIRMWARE_INSTALL_TARGET = NO
P4_FIRMWARE_INSTALL_IMAGES = YES

define P4_FIRMWARE_INSTALL_IMAGES_CMDS
	$(INSTALL) -m 0644 $(@D)/spl-p4-200.bin $(BINARIES_DIR)/spl-p4-200.bin
	$(INSTALL) -m 0644 $(@D)/uboot-p4.bin $(BINARIES_DIR)/uboot-p4.bin
endef

$(eval $(generic-package))
