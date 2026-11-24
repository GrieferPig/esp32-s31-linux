################################################################################
#
# s31-wavesynth
#
################################################################################

S31_WAVESYNTH_VERSION = 717398faecc4554a2ac040ffb09235a71edf6ba1
S31_WAVESYNTH_SITE = $(call github,GrieferPig,MyLittleWavesynth,$(S31_WAVESYNTH_VERSION))
S31_WAVESYNTH_DEPENDENCIES = alsa-lib
S31_WAVESYNTH_APP = $(BR2_EXTERNAL_ESP32_S31_PATH)/package/s31-wavesynth/src

define S31_WAVESYNTH_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -isystem $(@D) \
		$(S31_WAVESYNTH_APP)/wavesynth.c -lasound -o $(@D)/s31-wavesynth
endef

define S31_WAVESYNTH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/s31-wavesynth $(TARGET_DIR)/usr/bin/s31-wavesynth
	$(INSTALL) -D -m 0755 $(S31_WAVESYNTH_APP)/S96wavesynth $(TARGET_DIR)/etc/init.d/S96wavesynth
endef

$(eval $(generic-package))
