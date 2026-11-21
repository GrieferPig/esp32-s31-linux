################################################################################
#
# btstack-s31
#
################################################################################

BTSTACK_S31_VERSION = 431d58d5613fd8fae38afe50282b25302de84bf7
BTSTACK_S31_SITE = $(call github,bluekitchen,btstack,$(BTSTACK_S31_VERSION))
BTSTACK_S31_LICENSE = BTstack License (non-commercial)
BTSTACK_S31_LICENSE_FILES = LICENSE
BTSTACK_S31_DEPENDENCIES = bluez5_utils-headers host-python3
BTSTACK_S31_SUBDIR = port/linux
BTSTACK_S31_SUPPORTS_IN_SOURCE_BUILD = NO
BTSTACK_S31_EXAMPLES = a2dp_sink_demo gatt_counter hfp_hf_demo
BTSTACK_S31_BUILD_OPTS = --target $(BTSTACK_S31_EXAMPLES)
# The upstream Linux port uses standard HCI headers; no BlueZ daemon is needed.
# musl warns about upstream sys/errno.h; keep the warning without a source patch.
BTSTACK_S31_CONF_OPTS = \
	-DCMAKE_C_FLAGS="$(TARGET_CFLAGS) -Wno-error=cpp -ffunction-sections -fdata-sections" \
	-DCMAKE_EXE_LINKER_FLAGS="$(TARGET_LDFLAGS) -Wl,--gc-sections" \
	-DPython3_EXECUTABLE=$(HOST_DIR)/bin/python3 \
	-DCMAKE_DISABLE_FIND_PACKAGE_ALSA=TRUE \
	-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=TRUE

define BTSTACK_S31_CONFIGURE_HEADER
	$(INSTALL) -m 0644 $(BR2_EXTERNAL_ESP32_S31_PATH)/package/btstack-s31/s31_btstack_config.h \
		$(@D)/port/linux/btstack_config.h
endef
BTSTACK_S31_PRE_CONFIGURE_HOOKS += BTSTACK_S31_CONFIGURE_HEADER

define BTSTACK_S31_INSTALL_TARGET_CMDS
	for app in $(BTSTACK_S31_EXAMPLES); do \
		$(INSTALL) -D -m 0755 $(BTSTACK_S31_BUILDDIR)/$$app $(TARGET_DIR)/usr/bin/$$app; \
	done
endef

$(eval $(cmake-package))
