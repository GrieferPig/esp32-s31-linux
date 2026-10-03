# Load shared versions before package files: generic-package derives its name
# from the last entry in MAKEFILE_LIST at evaluation time.
include $(BR2_EXTERNAL_ESP32_S31_PATH)/../configs/build-versions.mk

include $(sort $(wildcard $(BR2_EXTERNAL_ESP32_S31_PATH)/package/*/*.mk))
