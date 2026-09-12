################################################################################
#
# s31-tools
#
################################################################################

S31_TOOLS_VERSION = 1.0
S31_TOOLS_SITE = $(BR2_EXTERNAL_ESP32_S31_PATH)/../rootfs
S31_TOOLS_SITE_METHOD = local
S31_TOOLS_LICENSE = GPL-2.0-only
S31_TOOLS_DEPENDENCIES = dtc esp-simd
S31_TOOLS_INSTALL_STAGING = YES
S31_TOOLS_OVERLAY_UAPI = $(BR2_EXTERNAL_ESP32_S31_PATH)/../linux-esp32-s31/include/uapi/linux/esp32s31-overlay.h
S31_TOOLS_EXT_CFLAGS = -march=rv32imafbc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs_xesploop_xespv2p2 -mespv-spec=2p2

define S31_TOOLS_BUILD_CMDS
	$(INSTALL) -D -m 0644 $(S31_TOOLS_OVERLAY_UAPI) $(@D)/include/linux/esp32s31-overlay.h
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/segfault.c -o $(@D)/segfault
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/forktest.c -o $(@D)/forktest
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/membench.c -o $(@D)/membench
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/s31_crypto_test.c -o $(@D)/s31-crypto-test
	$(TARGET_CC) $(TARGET_CFLAGS) $(S31_TOOLS_EXT_CFLAGS) -c $(@D)/s31_ext_test.S \
		-o $(@D)/s31_ext_test.o
	$(TARGET_CC) $(TARGET_CFLAGS) $(S31_TOOLS_EXT_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/s31_ext_test.c $(@D)/s31_ext_test.o \
		-L$(STAGING_DIR)/usr/lib -lesp-simd -o $(@D)/s31-ext-test
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-fno-builtin-memcpy -fno-builtin-memset \
		-fno-builtin-memmove $(@D)/s31_libc_test.c \
		-o $(@D)/s31-libc-test
	$(TARGET_CC) $(TARGET_CFLAGS) $(S31_TOOLS_EXT_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/s31_mem_compare.c -L$(STAGING_DIR)/usr/lib -lesp-simd \
		-o $(@D)/s31-mem-compare
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -fno-builtin \
		$(@D)/s31_string_bench.c -I$(STAGING_DIR)/usr/include \
		-L$(STAGING_DIR)/usr/lib -lesp-simd -o $(@D)/s31-string-bench
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-I$(@D)/include -I$(STAGING_DIR)/usr/include \
		$(@D)/s31_overlay.c -lfdt -o $(@D)/s31-overlay
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/s31_modload.c -o $(@D)/s31-modload
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/s31_hil_io.c -o $(@D)/s31-hil-io
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -pthread \
		$(@D)/s31_tlb_stress.c -o $(@D)/s31-tlb-stress
endef

define S31_TOOLS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/segfault $(TARGET_DIR)/usr/sbin/segfault
	$(INSTALL) -D -m 0755 $(@D)/forktest $(TARGET_DIR)/usr/sbin/forktest
	$(INSTALL) -D -m 0755 $(@D)/membench $(TARGET_DIR)/usr/sbin/membench
	$(INSTALL) -D -m 0755 $(@D)/s31-crypto-test \
		$(TARGET_DIR)/usr/sbin/s31-crypto-test
	$(INSTALL) -D -m 0755 $(@D)/s31-ext-test \
		$(TARGET_DIR)/usr/sbin/s31-ext-test
	$(INSTALL) -D -m 0755 $(@D)/s31-libc-test \
		$(TARGET_DIR)/usr/sbin/s31-libc-test
	$(INSTALL) -D -m 0755 $(@D)/s31-mem-compare \
		$(TARGET_DIR)/usr/sbin/s31-mem-compare
	$(INSTALL) -D -m 0755 $(@D)/s31-string-bench \
		$(TARGET_DIR)/usr/sbin/s31-string-bench
	$(INSTALL) -D -m 0755 $(@D)/s31-overlay \
		$(TARGET_DIR)/usr/sbin/s31-overlay
	$(INSTALL) -D -m 0755 $(@D)/s31-modload \
		$(TARGET_DIR)/usr/sbin/s31-modload
	$(INSTALL) -D -m 0755 $(@D)/s31-hil-io \
		$(TARGET_DIR)/usr/sbin/s31-hil-io
	$(INSTALL) -D -m 0755 $(@D)/s31-tlb-stress \
		$(TARGET_DIR)/usr/sbin/s31-tlb-stress
	$(INSTALL) -D -m 0755 \
		$(BR2_EXTERNAL_ESP32_S31_PATH)/../tools/cpu_sample.sh \
		$(TARGET_DIR)/usr/sbin/s31-cpu-sample
endef

define S31_TOOLS_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(S31_TOOLS_OVERLAY_UAPI) \
		$(STAGING_DIR)/usr/include/linux/esp32s31-overlay.h
endef

$(eval $(generic-package))
