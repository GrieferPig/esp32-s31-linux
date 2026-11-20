# P4 rev1.3 component target. The Linux/DTB FIT is assembled separately.
P4_CROSS_COMPILE ?= riscv32-esp-elf-
P4_FLASH_MODE ?= dio
P4_FLASH_FREQ ?= 40m
P4_PSRAM_MHZ ?= 80
ifeq ($(P4_PSRAM_MHZ),200)
P4_SPL_FRAGMENT := --fragment "$(ROOT)/configs/uboot-p4-psram200.config"
else ifneq ($(P4_PSRAM_MHZ),80)
$(error P4_PSRAM_MHZ must be 80 or 200)
endif

P4_SPL_OUT := $(UBOOT_OUT)/spl-$(P4_PSRAM_MHZ)
P4_PROPER_OUT := $(UBOOT_OUT)/proper

.PHONY: help uboot bootloader p4-fit p4-fit-inputs p4-uboot-compiler-check
help:
	@printf '%s\n' 'ESP32-P4 rev1.3: make uboot TARGET=esp32p4 [P4_CROSS_COMPILE=...] [ESPTOOL=...]' \
		'Package with an existing P4 kernel: make p4-fit TARGET=esp32p4 P4_LINUX_IMAGE=... P4_LINUX_DTB=...' \
		'Build a P4 NOMMU initramfs: make p4-rootfs TARGET=esp32p4'
bootloader: uboot
p4-uboot-compiler-check:
	@command -v "$(P4_CROSS_COMPILE)gcc" >/dev/null || { echo "ERROR: missing $(P4_CROSS_COMPILE)gcc; set P4_CROSS_COMPILE" >&2; exit 1; }

define p4_build_uboot
	python3 "$(ROOT)/tools/build/configure.py" native --kind uboot --source "$(UBOOT_DIR)" --output "$(1)" \
		--defconfig "$(2)" --compiler "$$(command -v "$(P4_CROSS_COMPILE)gcc")" --cross "$(P4_CROSS_COMPILE)" $(3)
	PYTHONPATH="$(UBOOT_PYTHONPATH)$${PYTHONPATH:+:$$PYTHONPATH}" python3 "$(ROOT)/tools/build/host_python.py" \
		--python "$(HOST_PYTHON)" --shim "$(1)/.host-python" -- \
		$(MAKE) -C "$(UBOOT_DIR)" O="$(1)" ARCH=riscv CROSS_COMPILE="$(P4_CROSS_COMPILE)" \
		PYTHON="$(HOST_PYTHON)" PYTHON3="$(HOST_PYTHON)" -j$(JOBS)
endef

uboot: uboot-esptool-check p4-uboot-compiler-check
	@grep -qx 'config ESP32P4_SPL_PSRAM_200MHZ' "$(UBOOT_DIR)/board/espressif/esp32p4/Kconfig" || { echo "ERROR: P4 needs the native SPL U-Boot revision; update the U-Boot submodule." >&2; exit 1; }
	$(call p4_build_uboot,$(P4_SPL_OUT),espressif_esp32p4_spl_defconfig,$(P4_SPL_FRAGMENT))
	@if [ "$(P4_PSRAM_MHZ)" = 200 ]; then grep -qx 'CONFIG_ESP32P4_SPL_PSRAM_200MHZ=y' "$(P4_SPL_OUT)/.config"; \
	else ! grep -qx 'CONFIG_ESP32P4_SPL_PSRAM_200MHZ=y' "$(P4_SPL_OUT)/.config"; fi
	$(call p4_build_uboot,$(P4_PROPER_OUT),espressif_esp32p4_sram_defconfig)
	@set -eu; mkdir -p "$(IMAGES_DIR)"; \
	test "$$("$(P4_CROSS_COMPILE)nm" "$(P4_PROPER_OUT)/u-boot" | awk '$$3 == "_start" { print $$1 }')" = 4ff00000; \
	spl_entry=$$("$(P4_CROSS_COMPILE)nm" "$(P4_SPL_OUT)/spl/u-boot-spl" | awk '$$3 == "_start" { print $$1 }'); \
	test "0x$$spl_entry" = "$$(sed -n 's/^CONFIG_SPL_TEXT_BASE=//p' "$(P4_SPL_OUT)/.config")"; \
	sh "$(ROOT)/tools/build/wrap_esp_spl.sh" "$(ESPTOOL)" "$(P4_CROSS_COMPILE)objcopy" \
		esp32p4 "0x$$spl_entry" "$(P4_FLASH_MODE)" "$(P4_FLASH_FREQ)" \
		"$(P4_SPL_OUT)/spl/u-boot-spl-dtb.bin" "$(IMAGES_DIR)/spl-p4-$(P4_PSRAM_MHZ).bin" \
		--min-rev-full 103 --max-rev-full 103; \
	cp "$(P4_PROPER_OUT)/u-boot.bin" "$(IMAGES_DIR)/uboot-p4.bin"

p4-fit-inputs:
	@test -f "$(P4_LINUX_IMAGE)" && test -f "$(P4_LINUX_DTB)" || { echo 'ERROR: set P4_LINUX_IMAGE and P4_LINUX_DTB to existing P4 artifacts.' >&2; exit 1; }

p4-fit: p4-fit-inputs uboot
	python3 "$(UBOOT_DIR)/board/espressif/esp32p4/tools/pack-fit.py" --mkimage "$(P4_PROPER_OUT)/tools/mkimage" \
		"$(IMAGES_DIR)/spl-p4-$(P4_PSRAM_MHZ).bin" "$(IMAGES_DIR)/uboot-p4.bin" \
		"$(P4_LINUX_IMAGE)" "$(P4_LINUX_DTB)" "$(IMAGES_DIR)/fit-$(P4_PSRAM_MHZ)"

# Board-specific Buildroot output. Keep the S31 rootfs rules independent.
P4_COREMARK_CC ?=
P4_BUILDROOT_MAKE = $(MAKE) -C "$(BUILDROOT_DIR)" O="$(BUILDROOT_OUT)" \
	BR2_EXTERNAL="$(BUILDROOT_EXTERNAL)" BR2_DL_DIR="$(BUILDROOT_DL_DIR)" BR2_JLEVEL="$(JOBS)"

.PHONY: rootfs p4-rootfs p4-rootfs-config
rootfs: p4-rootfs

p4-rootfs-config:
	$(P4_BUILDROOT_MAKE) esp32p4_rootfs_defconfig

p4-rootfs: p4-rootfs-config
	$(P4_BUILDROOT_MAKE) COREMARK_COMPUTE_CC="$(P4_COREMARK_CC)"
	@mkdir -p "$(IMAGES_DIR)"
	cp "$(BUILDROOT_OUT)/images/rootfs.cpio" "$(IMAGES_DIR)/rootfs-p4.cpio"
