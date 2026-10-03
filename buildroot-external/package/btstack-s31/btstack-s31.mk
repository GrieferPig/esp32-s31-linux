################################################################################
#
# btstack-s31
#
################################################################################

BTSTACK_S31_VERSION = $(BTSTACK_REF)
S31_BTSTACK_SOURCE ?= $(BR2_EXTERNAL_ESP32_S31_PATH)/../cache/sources/btstack
BTSTACK_S31_SITE = $(abspath $(S31_BTSTACK_SOURCE))
BTSTACK_S31_SITE_METHOD = local
BTSTACK_S31_LICENSE = BTstack License (non-commercial)
BTSTACK_S31_LICENSE_FILES = LICENSE

BTSTACK_S31_OPTIMIZATION = -Os

BTSTACK_S31_INCLUDES = \
	-I$(@D)/port/linux \
	-I$(@D)/src \
	-I$(@D)/3rd-party/bluedroid/decoder/include \
	-I$(@D)/3rd-party/bluedroid/encoder/include \
	-I$(@D)/3rd-party/lc3-google/include \
	-I$(@D)/3rd-party/md5 \
	-I$(@D)/3rd-party/yxml \
	-I$(@D)/platform/embedded \
	-I$(@D)/platform/posix \
	-I$(@D)/platform/linux

BTSTACK_S31_CFLAGS = $(filter-out -O%,$(TARGET_CFLAGS)) \
	$(filter-out -O%,$(call qstrip,$(BR2_TARGET_OPTIMIZATION))) \
	-DS31_A2DP_SBC_MAX_BITPOOL=53 \
	-DS31_A2DP_SBC_CAPABILITIES_0=0x21 \
	-DS31_BTSTACK_TRANSPORT_ONLY=1 \
	-DS31_BTSTACK_FORCE_CLASSIC=1 \
	-ffunction-sections -fdata-sections $(BTSTACK_S31_OPTIMIZATION) \
	$(BTSTACK_S31_INCLUDES)

define BTSTACK_S31_PREPARE_LOCAL_SOURCE
	$(APPLY_PATCHES) $(@D) \
		$(BR2_EXTERNAL_ESP32_S31_PATH)/package/btstack-s31 '*.patch'
	$(INSTALL) -D -m 0644 \
		$(BR2_EXTERNAL_ESP32_S31_PATH)/package/btstack-s31/s31_btstack_config.h \
		$(@D)/port/linux/btstack_config.h
endef
BTSTACK_S31_POST_RSYNC_HOOKS += BTSTACK_S31_PREPARE_LOCAL_SOURCE

define BTSTACK_S31_BUILD_CMDS
	rm -rf $(@D)/s31-build
	mkdir -p $(@D)/s31-build/objects
	set -e; \
	for source in \
		$(@D)/src/*.c \
		$(@D)/src/ble/*.c \
		$(@D)/src/classic/*.c \
		$(@D)/platform/posix/btstack_run_loop_posix.c \
		$(@D)/platform/posix/btstack_signal.c \
		$(@D)/platform/posix/btstack_stdin_posix.c \
		$(@D)/platform/posix/btstack_tlv_posix.c \
		$(@D)/platform/posix/hci_dump_posix_fs.c \
		$(@D)/platform/linux/hci_transport_linux.c \
		$(@D)/port/linux/main.c; do \
		object="$(@D)/s31-build/objects/$$(printf '%s' "$${source#$(@D)/}" | tr '/.' '__').o"; \
		$(TARGET_CC) $(BTSTACK_S31_CFLAGS) -c "$$source" -o "$$object"; \
	done
	$(TARGET_AR) rcs $(@D)/s31-build/libbtstack-s31.a \
		$(@D)/s31-build/objects/*.o
	$(TARGET_CC) $(BTSTACK_S31_CFLAGS) -c \
		$(@D)/example/a2dp_sink_demo.c \
		-o $(@D)/s31-build/a2dp_sink_demo.o
	$(TARGET_CC) $(TARGET_LDFLAGS) -Wl,--gc-sections \
		-o $(@D)/s31-build/s31-btstack-a2dp \
		$(@D)/s31-build/a2dp_sink_demo.o \
		$(@D)/s31-build/libbtstack-s31.a -lpthread -lm
endef

define BTSTACK_S31_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/s31-build/s31-btstack-a2dp \
		$(TARGET_DIR)/usr/sbin/s31-btstack-a2dp
	$(INSTALL) -D -m 0755 \
		$(BR2_EXTERNAL_ESP32_S31_PATH)/package/btstack-s31/S40btstack \
		$(TARGET_DIR)/etc/init.d/S40btstack
endef

$(eval $(generic-package))
