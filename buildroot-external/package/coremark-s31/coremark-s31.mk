################################################################################
#
# coremark-s31
#
################################################################################

COREMARK_S31_VERSION = 1.0
COREMARK_S31_SITE = $(BR2_EXTERNAL_ESP32_S31_PATH)/../coremark
COREMARK_S31_SITE_METHOD = local
COREMARK_S31_LICENSE = Apache-2.0
COREMARK_S31_LICENSE_FILES = LICENSE.md

define COREMARK_S31_BUILD_CMDS
	mkdir -p $(@D)/build
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		PORT_DIR=linux OPATH="$(@D)/build/" \
		CC="$(TARGET_CC)" NO_LIBRT=1 ITERATIONS=0 REBUILD=1 \
		XCFLAGS="$(TARGET_CFLAGS) -include $(COREMARK_S31_PKGDIR)/coremark-monotonic.h" compile
endef

define COREMARK_S31_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/coremark.exe \
		$(TARGET_DIR)/usr/sbin/coremark
endef

$(eval $(generic-package))
