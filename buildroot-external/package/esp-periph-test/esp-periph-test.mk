################################################################################
#
# esp-periph-test
#
################################################################################

ESP_PERIPH_TEST_VERSION = 1.2
ESP_PERIPH_TEST_SITE = $(BR2_EXTERNAL_ESP32_S31_PATH)/../rootfs
ESP_PERIPH_TEST_SITE_METHOD = local
ESP_PERIPH_TEST_LICENSE = GPL-2.0-only

define ESP_PERIPH_TEST_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/esp_periph_test.c -o $(@D)/esp-periph-test
endef

define ESP_PERIPH_TEST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/esp-periph-test \
		$(TARGET_DIR)/usr/sbin/esp-periph-test
endef

$(eval $(generic-package))
