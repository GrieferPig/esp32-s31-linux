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
BTSTACK_S31_DEPENDENCIES = bluez5_utils-headers
BTSTACK_S31_BOARD_SOURCE = $(BR2_EXTERNAL_ESP32_S31_PATH)/../rootfs

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
	-ffunction-sections -fdata-sections $(BTSTACK_S31_OPTIMIZATION) \
	$(BTSTACK_S31_INCLUDES)

define BTSTACK_S31_PREPARE_LOCAL_SOURCE
	mkdir -p $(@D)/s31-board
	$(INSTALL) -m 0644 $(BTSTACK_S31_BOARD_SOURCE)/s31_btstack.c $(@D)/s31-board/
	$(INSTALL) -m 0644 $(BTSTACK_S31_BOARD_SOURCE)/s31_btstack_transport.c $(@D)/s31-board/
	$(INSTALL) -D -m 0644 \
		$(BR2_EXTERNAL_ESP32_S31_PATH)/package/btstack-s31/s31_btstack_config.h \
		$(@D)/port/linux/btstack_config.h
endef
BTSTACK_S31_POST_RSYNC_HOOKS += BTSTACK_S31_PREPARE_LOCAL_SOURCE

define BTSTACK_S31_BUILD_LIBRARY
	rm -rf $(@D)/s31-build/$(1)
	mkdir -p $(@D)/s31-build/$(1)/objects
	set -e; \
	for source in \
		$(@D)/src/*.c \
		$(@D)/src/ble/*.c \
		$(@D)/src/classic/*.c \
		$(@D)/platform/posix/btstack_run_loop_posix.c \
		$(@D)/platform/posix/btstack_signal.c \
		$(@D)/platform/posix/btstack_stdin_posix.c \
		$(@D)/platform/posix/btstack_tlv_posix.c \
		$(@D)/s31-board/s31_btstack_transport.c; do \
		object="$(@D)/s31-build/$(1)/objects/$$(printf '%s' "$${source#$(@D)/}" | tr '/.' '__').o"; \
		$(TARGET_CC) $(filter-out -O%,$(BTSTACK_S31_CFLAGS)) -$(2) $(3) -c "$$source" -o "$$object"; \
	done
	$(TARGET_AR) rcs $(@D)/s31-build/$(1)/libbtstack-s31.a \
		$(@D)/s31-build/$(1)/objects/*.o
endef

define BTSTACK_S31_BUILD_VARIANT
	$(call BTSTACK_S31_BUILD_LIBRARY,$(1),$(1),)
	$(TARGET_CC) $(filter-out -O%,$(BTSTACK_S31_CFLAGS)) -$(1) -c \
		$(@D)/s31-board/s31_btstack.c \
		-o $(@D)/s31-build/$(1)/s31_btstack.o
	$(TARGET_CC) $(TARGET_LDFLAGS) -Wl,--gc-sections \
		-o $(@D)/s31-build/$(1)/s31-btstack-a2dp \
		$(@D)/s31-build/$(1)/s31_btstack.o \
		$(@D)/s31-build/$(1)/libbtstack-s31.a -lpthread -lm
endef

define BTSTACK_S31_BUILD_CMDS
	rm -rf $(@D)/s31-build
	$(call BTSTACK_S31_BUILD_VARIANT,Os)
endef

define BTSTACK_S31_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/s31-build/Os/s31-btstack-a2dp \
		$(TARGET_DIR)/usr/sbin/s31-btstack-a2dp
	$(INSTALL) -D -m 0755 \
		$(BR2_EXTERNAL_ESP32_S31_PATH)/package/btstack-s31/S40btstack \
		$(TARGET_DIR)/etc/init.d/S40btstack
endef

$(eval $(generic-package))
