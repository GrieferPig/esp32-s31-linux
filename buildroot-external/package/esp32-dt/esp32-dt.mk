################################################################################
#
# esp32-dt
#
################################################################################

ESP32_DT_VERSION = 1.0
ESP32_DT_SITE = $(BR2_EXTERNAL_ESP32_S31_PATH)/package/esp32-dt/src
ESP32_DT_SITE_METHOD = local
ESP32_DT_LICENSE = GPL-2.0-only
ESP32_DT_DEPENDENCIES = linux

# Link the kernel source tree's portable libfdt sources. The Buildroot dtc
# package requires shared libraries, which are unavailable on P4 NOMMU.
ESP32_DT_LIBFDT = $(LINUX_DIR)/scripts/dtc/libfdt

define ESP32_DT_BUILD_CMDS
	$(INSTALL) -m 0644 $(LINUX_DIR)/include/uapi/linux/esp32s31-overlay.h $(@D)/esp32s31-overlay.h
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -I$(ESP32_DT_LIBFDT) \
		$(@D)/esp32-dt.c $(ESP32_DT_LIBFDT)/*.c -o $(@D)/esp32-dt
endef

define ESP32_DT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/esp32-dt $(TARGET_DIR)/usr/sbin/esp32-dt
endef

$(eval $(generic-package))
