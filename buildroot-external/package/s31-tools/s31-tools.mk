################################################################################
#
# s31-tools
#
################################################################################

S31_TOOLS_VERSION = 1.0
S31_TOOLS_SITE = $(BR2_EXTERNAL_ESP32_S31_PATH)/../rootfs
S31_TOOLS_SITE_METHOD = local
S31_TOOLS_LICENSE = GPL-2.0-only
S31_TOOLS_DEPENDENCIES = dtc

S31_TOOLS_ESP_HOSTED_DIR = $(BR2_EXTERNAL_ESP32_S31_PATH)/../esp-hosted-fg
S31_TOOLS_PROTO_DIR = $(S31_TOOLS_ESP_HOSTED_DIR)/common/proto
S31_TOOLS_PROTOBUF_DIR = $(S31_TOOLS_ESP_HOSTED_DIR)/common/protobuf-c

define S31_TOOLS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/segfault.c -o $(@D)/segfault
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/forktest.c -o $(@D)/forktest
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/membench.c -o $(@D)/membench
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/s31_crypto_test.c -o $(@D)/s31-crypto-test
	$(TARGET_CC) $(TARGET_CFLAGS) -c $(@D)/s31_ext_test.S \
		-o $(@D)/s31_ext_test.o
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/s31_ext_test.c $(@D)/s31_ext_test.o \
		-o $(@D)/s31-ext-test
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-fno-builtin-memcpy -fno-builtin-memset \
		-fno-builtin-memmove $(@D)/s31_libc_test.c \
		-o $(@D)/s31-libc-test
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/s31_mem_compare.c -o $(@D)/s31-mem-compare
	$(TARGET_CC) $(TARGET_CFLAGS) -c $(@D)/s31_xespv_memops.S \
		-o $(@D)/s31_xespv_memops.o
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -fno-builtin \
		$(@D)/s31_string_bench.c $(@D)/s31_xespv_memops.o \
		-o $(@D)/s31-string-bench
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-I$(STAGING_DIR)/usr/include \
		$(@D)/s31_overlay.c -lfdt -o $(@D)/s31-overlay
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/ble_scan.c -o $(@D)/ble-scan
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/s31_clock_compare.c -o $(@D)/s31-clock-compare
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-I$(BR2_EXTERNAL_ESP32_S31_PATH)/../shared \
		$(@D)/s31_freertos_mem.c -o $(@D)/s31-freertos-mem
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-I$(S31_TOOLS_PROTO_DIR) \
		-I$(S31_TOOLS_PROTOBUF_DIR) \
		$(@D)/esp_hosted_ctl.c \
		$(S31_TOOLS_PROTO_DIR)/esp_hosted_rpc.pb-c.c \
		$(S31_TOOLS_PROTOBUF_DIR)/protobuf-c/protobuf-c.c \
		-o $(@D)/esp-hosted-ctl
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
	$(INSTALL) -D -m 0755 $(@D)/ble-scan $(TARGET_DIR)/usr/sbin/ble-scan
	$(INSTALL) -D -m 0755 $(@D)/s31-clock-compare \
		$(TARGET_DIR)/usr/sbin/s31-clock-compare
	$(INSTALL) -D -m 0755 $(@D)/s31-freertos-mem \
		$(TARGET_DIR)/usr/sbin/s31-freertos-mem
	$(INSTALL) -D -m 0755 $(@D)/esp-hosted-ctl \
		$(TARGET_DIR)/usr/sbin/esp-hosted-ctl
	ln -sfn esp-hosted-ctl $(TARGET_DIR)/usr/sbin/test.out
endef

$(eval $(generic-package))
