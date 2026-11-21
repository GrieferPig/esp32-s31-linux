# P4 rev1.3 component target. The Linux/DTB FIT is assembled separately.
P4_CROSS_COMPILE ?= riscv32-esp-elf-
P4_FLASH_MODE ?= dio
P4_FLASH_FREQ ?= 40m
# The 80 MHz profile can starve the Waveshare DSI scanout FIFO.
P4_PSRAM_MHZ ?= 200
ifeq ($(P4_PSRAM_MHZ),200)
P4_SPL_FRAGMENT := --fragment "$(UBOOT_DIR)/board/espressif/esp32p4/psram200.config"
else ifneq ($(P4_PSRAM_MHZ),80)
$(error P4_PSRAM_MHZ must be 80 or 200)
endif

P4_UBOOT_OUT := $(UBOOT_OUT)/p4-$(P4_PSRAM_MHZ)
P4_BOOT_DIR ?=
ifeq ($(strip $(P4_BOOT_DIR)),)
P4_PACK_BOOT_DIR := $(IMAGES_DIR)
P4_BOOT_PREREQUISITE := uboot
P4_MKIMAGE ?= $(P4_UBOOT_OUT)/tools/mkimage
else
P4_PACK_BOOT_DIR := $(abspath $(P4_BOOT_DIR))
P4_BOOT_PREREQUISITE := p4-release-boot-inputs
P4_MKIMAGE ?= mkimage
endif
P4_PACK_SPL := $(P4_PACK_BOOT_DIR)/spl-p4-$(P4_PSRAM_MHZ).bin
P4_PACK_UBOOT := $(P4_PACK_BOOT_DIR)/uboot-p4.bin

.PHONY: help uboot bootloader p4-fit p4-fit-inputs p4-uboot-compiler-check p4-release-boot-inputs
help:
	@printf '%s\n' 'ESP32-P4 rev1.3: make uboot TARGET=esp32p4 [P4_CROSS_COMPILE=...] [ESPTOOL=...]' \
		'Package with an existing P4 kernel: make p4-fit TARGET=esp32p4 P4_LINUX_IMAGE=... P4_LINUX_DTB=... P4_ROOTFS_IMAGE=...' \
		'Build the complete P4 NOR image: make image TARGET=esp32p4 [P4_BOOT_DIR=/path/to/verified/boot-inputs]' \
		'Build the P4 SquashFS rootfs: make p4-rootfs TARGET=esp32p4'
bootloader: uboot
p4-uboot-compiler-check:
	@command -v "$(P4_CROSS_COMPILE)gcc" >/dev/null || { echo "ERROR: missing $(P4_CROSS_COMPILE)gcc; set P4_CROSS_COMPILE" >&2; exit 1; }

uboot: uboot-esptool-check p4-uboot-compiler-check
	@grep -qx 'config ESP32P4_SPL_PSRAM_200MHZ' "$(UBOOT_DIR)/board/espressif/esp32p4/Kconfig" || { echo "ERROR: P4 needs the native SPL U-Boot revision; update the U-Boot submodule." >&2; exit 1; }
	python3 "$(ROOT)/tools/build/configure.py" native --kind uboot --source "$(UBOOT_DIR)" --output "$(P4_UBOOT_OUT)" \
		--defconfig espressif_esp32p4_defconfig --compiler "$$(command -v "$(P4_CROSS_COMPILE)gcc")" --cross "$(P4_CROSS_COMPILE)" $(P4_SPL_FRAGMENT)
	PYTHONPATH="$(UBOOT_PYTHONPATH)$${PYTHONPATH:+:$$PYTHONPATH}" python3 "$(ROOT)/tools/build/host_python.py" \
		--python "$(HOST_PYTHON)" --shim "$(P4_UBOOT_OUT)/.host-python" -- \
		$(MAKE) -C "$(UBOOT_DIR)" O="$(P4_UBOOT_OUT)" ARCH=riscv CROSS_COMPILE="$(P4_CROSS_COMPILE)" \
		PYTHON="$(HOST_PYTHON)" PYTHON3="$(HOST_PYTHON)" -j$(JOBS)
	@if [ "$(P4_PSRAM_MHZ)" = 200 ]; then grep -qx 'CONFIG_ESP32P4_SPL_PSRAM_200MHZ=y' "$(P4_UBOOT_OUT)/.config"; \
	else ! grep -qx 'CONFIG_ESP32P4_SPL_PSRAM_200MHZ=y' "$(P4_UBOOT_OUT)/.config"; fi
	@set -eu; mkdir -p "$(IMAGES_DIR)"; \
	test "$$("$(P4_CROSS_COMPILE)nm" "$(P4_UBOOT_OUT)/u-boot" | awk '$$3 == "_start" { print $$1 }')" = 4ff00000; \
	spl_entry=$$("$(P4_CROSS_COMPILE)nm" "$(P4_UBOOT_OUT)/spl/u-boot-spl" | awk '$$3 == "_start" { print $$1 }'); \
	test "0x$$spl_entry" = "$$(sed -n 's/^CONFIG_SPL_TEXT_BASE=//p' "$(P4_UBOOT_OUT)/.config")"; \
	sh "$(ROOT)/tools/build/wrap_esp_spl.sh" "$(ESPTOOL)" "$(P4_CROSS_COMPILE)objcopy" \
		esp32p4 "0x$$spl_entry" "$(P4_FLASH_MODE)" "$(P4_FLASH_FREQ)" \
		"$(P4_UBOOT_OUT)/spl/u-boot-spl-dtb.bin" "$(IMAGES_DIR)/spl-p4-$(P4_PSRAM_MHZ).bin" \
		--min-rev-full 103 --max-rev-full 103; \
	cp "$(P4_UBOOT_OUT)/u-boot.bin" "$(IMAGES_DIR)/uboot-p4.bin"

p4-fit-inputs:
	@test -f "$(P4_LINUX_IMAGE)" && test -f "$(P4_LINUX_DTB)" && test -f "$(P4_ROOTFS_IMAGE)" || { echo 'ERROR: provide P4 xipImage, DTB and SquashFS artifacts.' >&2; exit 1; }

p4-release-boot-inputs:
	@test -s "$(P4_PACK_SPL)" && test -s "$(P4_PACK_UBOOT)" || { \
		echo "ERROR: missing $(P4_PACK_SPL) or $(P4_PACK_UBOOT)" >&2; exit 1; }
	@command -v "$(P4_MKIMAGE)" >/dev/null || { echo "ERROR: missing $(P4_MKIMAGE)" >&2; exit 1; }

define p4_pack_fit
	python3 "$(ROOT)/ports/esp32p4-reconstructed/pack-fit.py" --mkimage "$(P4_MKIMAGE)" \
		--rootfs "$(P4_ROOTFS_IMAGE)" \
		"$(P4_PACK_SPL)" "$(P4_PACK_UBOOT)" \
		"$(P4_LINUX_IMAGE)" "$(P4_LINUX_DTB)" "$(IMAGES_DIR)/fit-$(P4_PSRAM_MHZ)"
endef

p4-fit: p4-fit-inputs $(P4_BOOT_PREREQUISITE)
	$(p4_pack_fit)

.PHONY: image
image: p4-rootfs $(P4_BOOT_PREREQUISITE)
	$(p4_pack_fit)

# Board-specific Buildroot output. Keep the S31 rootfs rules independent.
P4_BUILDROOT_MAKE = $(MAKE) -C "$(BUILDROOT_DIR)" O="$(BUILDROOT_OUT)" \
	BR2_EXTERNAL="$(BUILDROOT_EXTERNAL)" BR2_DL_DIR="$(BUILDROOT_DL_DIR)" BR2_JLEVEL="$(JOBS)"

.PHONY: rootfs p4-rootfs p4-rootfs-config
rootfs: p4-rootfs

p4-rootfs-config:
	$(P4_BUILDROOT_MAKE) esp32p4_rootfs_defconfig

p4-rootfs: p4-rootfs-config p4-linux
	$(P4_BUILDROOT_MAKE) \
		BR2_ROOTFS_OVERLAY="$(BUILDROOT_EXTERNAL)/board/esp32-p4/overlay $(P4_HOSTED_OVERLAY)"
	@mkdir -p "$(IMAGES_DIR)"
	cp "$(BUILDROOT_OUT)/images/rootfs.squashfs" "$(P4_ROOTFS_IMAGE)"

# ESP-Hosted-NG host SDK-free SDIO transport. The C6 firmware is separate.
P4_HOSTED_REV := 1df17f74d62eede4127785ae9414e03e48e62ebd
P4_HOSTED_SHA256 := 0f6f0c8f240f2abd318c14faaa76d6eeec6c8e50226b7a5625e085970b888db0
P4_HOSTED_ARCHIVE ?= $(CACHE_DIR)/downloads/esp-hosted-$(P4_HOSTED_REV).tar.gz
P4_HOSTED_SOURCE := $(BUILD_DIR)/esp-hosted-ng-$(P4_HOSTED_REV)
P4_HOSTED_PATCH := $(BUILDROOT_EXTERNAL)/board/esp32-p4/esp-hosted/0001-sdio-irq-handler-host-is-already-claimed.patch
P4_HOSTED_OVERLAY := $(STAGING_DIR)/esp-hosted
P4_ROOTFS_IMAGE ?= $(IMAGES_DIR)/rootfs-p4.squashfs
P4_PULSE_MODULES := rc-core.ko ir-esp32p4-rmt.ko
P4_BOARD_DTB := esp32p4-waveshare-wifi6-dev-kit.dtb
P4_GENERIC_DTB := esp32p4-generic.dtb
P4_LINUX_IMAGE ?= $(IMAGES_DIR)/xipImage-p4
P4_LINUX_DTB ?= $(IMAGES_DIR)/$(P4_BOARD_DTB)
P4_LINUX_MAKE = $(MAKE) -C "$(LINUX_DIR)" O="$(LINUX_OUT)" ARCH=riscv CROSS_COMPILE="$(P4_CROSS_COMPILE)" -j$(JOBS)

$(P4_HOSTED_ARCHIVE):
	@mkdir -p "$(@D)"
	curl --fail --location --output "$@.part" "https://github.com/espressif/esp-hosted/archive/$(P4_HOSTED_REV).tar.gz"
	printf '%s  %s\n' "$(P4_HOSTED_SHA256)" "$@.part" | sha256sum --check -
	mv "$@.part" "$@"

$(P4_HOSTED_SOURCE)/.prepared: $(P4_HOSTED_ARCHIVE) $(P4_HOSTED_PATCH)
	printf '%s  %s\n' "$(P4_HOSTED_SHA256)" "$(P4_HOSTED_ARCHIVE)" | sha256sum --check -
	mkdir -p "$(P4_HOSTED_SOURCE)"
	tar -xf "$(P4_HOSTED_ARCHIVE)" -C "$(P4_HOSTED_SOURCE)" --strip-components=3 \
		"esp-hosted-$(P4_HOSTED_REV)/esp_hosted_ng/host"
	patch -d "$(P4_HOSTED_SOURCE)" -p1 < "$(P4_HOSTED_PATCH)"
	touch "$@"

.PHONY: linux p4-linux
linux: p4-linux
p4-linux: p4-uboot-compiler-check $(P4_HOSTED_SOURCE)/.prepared | $(LINUX_OUT) $(IMAGES_DIR)
	$(P4_LINUX_MAKE) esp32p4_reconstructed_defconfig
	$(P4_LINUX_MAKE) xipImage modules espressif/$(P4_BOARD_DTB) espressif/$(P4_GENERIC_DTB)
	$(P4_LINUX_MAKE) M="$(P4_HOSTED_SOURCE)" target=sdio modules
	mkdir -p "$(P4_HOSTED_OVERLAY)/lib/modules"
	"$(P4_CROSS_COMPILE)strip" --strip-debug -o "$(P4_HOSTED_OVERLAY)/lib/modules/esp32_sdio.ko" "$(P4_HOSTED_SOURCE)/esp32_sdio.ko"
	@set -eu; for m in $(P4_PULSE_MODULES); do \
		"$(P4_CROSS_COMPILE)strip" --strip-debug -o "$(P4_HOSTED_OVERLAY)/lib/modules/$$m" \
			"$(LINUX_OUT)/drivers/media/rc/$$m"; \
	done
	cp "$(LINUX_OUT)/arch/riscv/boot/xipImage" "$(P4_LINUX_IMAGE)"
	cp "$(LINUX_OUT)/arch/riscv/boot/dts/espressif/$(P4_BOARD_DTB)" "$(P4_LINUX_DTB)"
	cp "$(LINUX_OUT)/arch/riscv/boot/dts/espressif/$(P4_GENERIC_DTB)" "$(IMAGES_DIR)/$(P4_GENERIC_DTB)"
