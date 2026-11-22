include $(sort $(wildcard $(BR2_EXTERNAL_ESP32_S31_PATH)/toolchain/*/*.mk))
include $(sort $(wildcard $(BR2_EXTERNAL_ESP32_S31_PATH)/package/*/*.mk))

ifeq ($(BR2_PACKAGE_P4_FIRMWARE),y)
# The P4 kernel uses the ESP compiler. Userspace uses Buildroot's NOMMU toolchain.
P4_CROSS_COMPILE ?= riscv32-esp-elf-
LINUX_MAKE_FLAGS += CROSS_COMPILE="$(P4_CROSS_COMPILE)"
# NOMMU userspace uses FLAT binaries. Kernel modules are still ELF.
LINUX_MAKE_FLAGS += INSTALL_MOD_STRIP=1
# Kbuild adds this include path for O= builds. Use the driver's existing
# kernel stdint shim when Buildroot builds inside the Linux source tree.
LINUX_MAKE_FLAGS += CFLAGS_esp32p4-dsi.o=-I$(LINUX_DIR)/drivers/video/fbdev
endif
