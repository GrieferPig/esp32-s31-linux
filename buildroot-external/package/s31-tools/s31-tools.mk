################################################################################
#
# s31-tools
#
################################################################################

S31_TOOLS_VERSION = 1.0
S31_TOOLS_SITE = $(BR2_EXTERNAL_ESP32_S31_PATH)/../rootfs
S31_TOOLS_SITE_METHOD = local
S31_TOOLS_LICENSE = GPL-2.0-only
S31_TOOLS_DEPENDENCIES = dtc openssl
S31_TOOLS_INSTALL_STAGING = YES
S31_TOOLS_OVERLAY_UAPI = $(BR2_EXTERNAL_ESP32_S31_PATH)/../linux-esp32-s31/include/uapi/linux/esp32s31-overlay.h

S31_TOOLS_ACCEL_UAPI = $(BR2_EXTERNAL_ESP32_S31_PATH)/../linux-esp32-s31/include/uapi/linux/esp32s31-accelerators.h

define S31_TOOLS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/s31_wifi_psk.c -lcrypto -o $(@D)/s31-wifi-psk
	$(INSTALL) -D -m 0644 $(S31_TOOLS_OVERLAY_UAPI) $(@D)/include/linux/esp32s31-overlay.h
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-I$(@D)/include -I$(STAGING_DIR)/usr/include \
		$(@D)/s31_overlay.c -lfdt -o $(@D)/s31-overlay
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/s31_modload.c -o $(@D)/s31-modload
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/s31_gpio.c -o $(@D)/s31-gpio
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/s31_config_archive.c -o $(@D)/s31-config-archive
endef

define S31_TOOLS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/s31-wifi-psk $(TARGET_DIR)/usr/sbin/s31-wifi-psk
	$(INSTALL) -D -m 0755 $(@D)/s31-overlay $(TARGET_DIR)/usr/sbin/s31-overlay
	$(INSTALL) -D -m 0755 $(@D)/s31-modload $(TARGET_DIR)/usr/sbin/s31-modload
	$(INSTALL) -D -m 0755 $(@D)/s31-gpio $(TARGET_DIR)/usr/sbin/s31-gpio
	$(INSTALL) -D -m 0755 $(@D)/s31-config-archive $(TARGET_DIR)/usr/sbin/s31-config-archive
endef

define S31_TOOLS_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(S31_TOOLS_ACCEL_UAPI) \
		$(STAGING_DIR)/usr/include/linux/esp32s31-accelerators.h
	$(INSTALL) -D -m 0644 $(S31_TOOLS_OVERLAY_UAPI) \
		$(STAGING_DIR)/usr/include/linux/esp32s31-overlay.h
endef

$(eval $(generic-package))
