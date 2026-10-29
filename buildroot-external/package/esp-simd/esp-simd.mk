################################################################################
#
# esp-simd
#
################################################################################

ESP_SIMD_VERSION = 1.0
ESP_SIMD_SITE = $(BR2_EXTERNAL_ESP32_S31_PATH)/../src/userspace/simd
ESP_SIMD_SITE_METHOD = local
ESP_SIMD_LICENSE = GPL-2.0-only
ESP_SIMD_INSTALL_STAGING = YES
ESP_SIMD_XESPV_CFLAGS = -march=rv32imafbc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs_xesploop_xespv2p2 -mespv-spec=2p2

define ESP_SIMD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -fPIC -I$(@D) \
		-c $(@D)/esp_simd.c -o $(@D)/esp_simd.o
	$(TARGET_CC) $(TARGET_CFLAGS) $(ESP_SIMD_XESPV_CFLAGS) -fPIC \
		-c $(@D)/s31_xespv_memops.S -o $(@D)/s31_xespv_memops.o
	$(TARGET_CC) $(TARGET_LDFLAGS) -shared -Wl,--no-undefined \
		-Wl,-soname,libesp-simd.so.1 -o $(@D)/libesp-simd.so.1.0.0 \
		$(@D)/esp_simd.o $(@D)/s31_xespv_memops.o
	$(TARGET_AR) rcs $(@D)/libesp-simd.a \
		$(@D)/esp_simd.o $(@D)/s31_xespv_memops.o
endef

define ESP_SIMD_INSTALL_LIBS
	$(INSTALL) -D -m 0755 $(@D)/libesp-simd.so.1.0.0 \
		$(1)/usr/lib/libesp-simd.so.1.0.0
	ln -sfn libesp-simd.so.1.0.0 $(1)/usr/lib/libesp-simd.so.1
	ln -sfn libesp-simd.so.1 $(1)/usr/lib/libesp-simd.so
	$(INSTALL) -D -m 0644 $(@D)/esp_simd.h $(1)/usr/include/esp_simd.h
endef

define ESP_SIMD_INSTALL_STAGING_CMDS
	$(call ESP_SIMD_INSTALL_LIBS,$(STAGING_DIR))
	$(INSTALL) -D -m 0644 $(@D)/libesp-simd.a \
		$(STAGING_DIR)/usr/lib/libesp-simd.a
endef

define ESP_SIMD_INSTALL_TARGET_CMDS
	$(call ESP_SIMD_INSTALL_LIBS,$(TARGET_DIR))
endef

$(eval $(generic-package))
