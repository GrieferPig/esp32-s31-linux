# SPDX-License-Identifier: BSD-2-Clause
S31_FIRMWARE_URL = $(call qstrip,$(BR2_PACKAGE_S31_FIRMWARE_ARCHIVE))
S31_FIRMWARE_SHA256 = $(call qstrip,$(BR2_PACKAGE_S31_FIRMWARE_SHA256))
S31_FIRMWARE_VERSION = $(S31_FIRMWARE_SHA256)
S31_FIRMWARE_SOURCE = $(notdir $(S31_FIRMWARE_URL))
S31_FIRMWARE_SITE = $(patsubst %/,%,$(dir $(S31_FIRMWARE_URL)))
S31_FIRMWARE_LICENSE = Proprietary, Apache-2.0, GPL-2.0, BSD-2-Clause
S31_FIRMWARE_LICENSE_FILES = source/firmware/radio/RADIO_BUNDLE_LICENSES.md source/THIRD_PARTY_NOTICES.md
S31_FIRMWARE_REDISTRIBUTE = NO
S31_FIRMWARE_INSTALL_IMAGES = YES
S31_FIRMWARE_STRIP_COMPONENTS = 0
# Host tools used by the board post-image hook.
S31_FIRMWARE_DEPENDENCIES = host-python3 host-python-pyelftools host-genimage
S31_FIRMWARE_PAYLOAD = $(S31_FIRMWARE_DIR)

define S31_FIRMWARE_WRITE_HASH
	$(Q)printf 'sha256  %s  %s\n' '$(S31_FIRMWARE_SHA256)' '$(S31_FIRMWARE_SOURCE)' > $(BASE_DIR)/.s31-firmware.hash
endef
S31_FIRMWARE_PRE_DOWNLOAD_HOOKS += S31_FIRMWARE_WRITE_HASH

define S31_FIRMWARE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/staging/overlay/lib/firmware/esp32s31/s31-lp-core.elf $(TARGET_DIR)/lib/firmware/esp32s31/s31-lp-core.elf
endef

define S31_FIRMWARE_INSTALL_IMAGES_CMDS
	$(INSTALL) -m 0644 $(@D)/images/spl_app.bin $(BINARIES_DIR)/spl_app.bin
	$(INSTALL) -m 0644 $(@D)/images/u-boot.itb $(BINARIES_DIR)/u-boot.itb
endef

$(eval $(generic-package))
# Set after generic-package, which defines its own list. The standard downloader
# consumes this hash before standard archive extraction.
S31_FIRMWARE_HASH_FILES += $(BASE_DIR)/.s31-firmware.hash
