################################################################################
#
# s31-tools
#
################################################################################

S31_TOOLS_VERSION = 1.0
S31_TOOLS_SITE = $(BR2_EXTERNAL_ESP32_S31_PATH)/package/s31-tools/src
S31_TOOLS_SITE_METHOD = local
S31_TOOLS_LICENSE = GPL-2.0-only
S31_TOOLS_DEPENDENCIES = dtc
S31_TOOLS_DEPENDENCIES += linux
S31_TOOLS_OVERLAY_UAPI = $(LINUX_DIR)/include/uapi/linux/esp32s31-overlay.h

define S31_TOOLS_BUILD_CMDS
	$(INSTALL) -D -m 0644 $(S31_TOOLS_OVERLAY_UAPI) $(@D)/include/linux/esp32s31-overlay.h
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-I$(@D)/include -I$(STAGING_DIR)/usr/include \
		$(@D)/s31_overlay.c -lfdt -o $(@D)/s31-overlay
endef

define S31_TOOLS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/s31-overlay $(TARGET_DIR)/usr/sbin/s31-overlay
endef

$(eval $(generic-package))
