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
		'Build Linux and the P4 Buildroot initramfs: make p4-linux TARGET=esp32p4' \
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
P4_COREMARK_CC ?= $(P4_CROSS_COMPILE)gcc
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

# ESP-Hosted-NG host SDK-free SDIO transport. The C6 firmware is separate.
P4_HOSTED_REV := 1df17f74d62eede4127785ae9414e03e48e62ebd
P4_HOSTED_SHA256 := 0f6f0c8f240f2abd318c14faaa76d6eeec6c8e50226b7a5625e085970b888db0
P4_HOSTED_ARCHIVE ?= $(CACHE_DIR)/downloads/esp-hosted-$(P4_HOSTED_REV).tar.gz
P4_HOSTED_SOURCE := $(BUILD_DIR)/esp-hosted-ng-$(P4_HOSTED_REV)
P4_HOSTED_PATCH := $(BUILDROOT_EXTERNAL)/board/esp32-p4/esp-hosted/0001-sdio-irq-handler-host-is-already-claimed.patch
P4_HOSTED_OVERLAY := $(STAGING_DIR)/esp-hosted
P4_BASE_INITRAMFS := $(IMAGES_DIR)/rootfs-p4.cpio
P4_HOSTED_INITRAMFS := $(IMAGES_DIR)/rootfs-p4-hosted.cpio
P4_PULSE_MODULES := rc-core.ko ir-esp32p4-rmt.ko
P4_BOARD_DTB := esp32p4-waveshare-touch-display-2.dtb
P4_LINUX_IMAGE ?= $(IMAGES_DIR)/Image-p4
P4_LINUX_DTB ?= $(IMAGES_DIR)/$(P4_BOARD_DTB)
P4_LINUX_MAKE = $(MAKE) -C "$(LINUX_DIR)" O="$(LINUX_OUT)" ARCH=riscv CROSS_COMPILE="$(P4_CROSS_COMPILE)" -j$(JOBS)
P4_LINUX_CONFIG = "$(LINUX_DIR)/scripts/config" --file "$(LINUX_OUT)/.config"

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
p4-linux: p4-uboot-compiler-check p4-rootfs $(P4_HOSTED_SOURCE)/.prepared | $(LINUX_OUT) $(IMAGES_DIR)
	$(P4_LINUX_MAKE) esp32p4_reconstructed_defconfig
	"$(LINUX_DIR)/scripts/kconfig/merge_config.sh" -m -r -O "$(LINUX_OUT)" "$(LINUX_OUT)/.config" \
		"$(LINUX_DIR)/arch/riscv/configs/esp32p4_native_peripherals.config"
	$(P4_LINUX_CONFIG) -e INITRAMFS_COMPRESSION_XZ --set-str INITRAMFS_SOURCE "$(P4_BASE_INITRAMFS)"
	$(P4_LINUX_MAKE) olddefconfig
	$(P4_LINUX_MAKE) Image modules espressif/$(P4_BOARD_DTB)
	$(P4_LINUX_MAKE) M="$(P4_HOSTED_SOURCE)" target=sdio modules
	mkdir -p "$(P4_HOSTED_OVERLAY)/lib/modules"
	"$(P4_CROSS_COMPILE)strip" --strip-debug -o "$(P4_HOSTED_OVERLAY)/lib/modules/esp32_sdio.ko" "$(P4_HOSTED_SOURCE)/esp32_sdio.ko"
	@set -eu; for m in $(P4_PULSE_MODULES); do \
		"$(P4_CROSS_COMPILE)strip" --strip-debug -o "$(P4_HOSTED_OVERLAY)/lib/modules/$$m" \
			"$(LINUX_OUT)/drivers/media/rc/$$m"; \
	done
	cd "$(P4_HOSTED_OVERLAY)" && find . -print0 | LC_ALL=C sort -z | cpio --null --quiet --reproducible -o --format=newc --owner=0:0 > "$(IMAGES_DIR)/p4-hosted-overlay.cpio"
	cat "$(P4_BASE_INITRAMFS)" "$(IMAGES_DIR)/p4-hosted-overlay.cpio" > "$(P4_HOSTED_INITRAMFS)"
	$(P4_LINUX_CONFIG) --set-str INITRAMFS_SOURCE "$(P4_HOSTED_INITRAMFS)"
	$(P4_LINUX_MAKE) olddefconfig
	$(P4_LINUX_MAKE) Image
	cp "$(LINUX_OUT)/arch/riscv/boot/Image" "$(P4_LINUX_IMAGE)"
	cp "$(LINUX_OUT)/arch/riscv/boot/dts/espressif/$(P4_BOARD_DTB)" "$(P4_LINUX_DTB)"
