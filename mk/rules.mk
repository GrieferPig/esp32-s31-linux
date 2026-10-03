.NOTPARALLEL:
include $(ROOT)/mk/toolchains.mk
include $(ROOT)/mk/boot.mk
include $(ROOT)/mk/radio.mk
include $(ROOT)/mk/linux.mk
include $(ROOT)/mk/rootfs.mk
include $(ROOT)/mk/images.mk
include $(ROOT)/mk/maintenance.mk
include $(ROOT)/mk/device.mk
include $(ROOT)/mk/checks.mk
.PHONY: help doctor fetch build all check check-artifacts radio-fs flash-image initramfs bootloader
help:
	@printf '%s\n' 'ESP32-S31: GNU Make public interface' '  make doctor | fetch | build | image | check' '  Full board configuration; DEBUG=1 optional' '  Components: linux uboot rootfs radio-image lp-firmware' '  Release: radio-package; device: flash-existing-all (never builds)' '  Outputs: out; cache survives clean; machine paths/jobs: local.mk'
doctor:
	@bash -c 'if [ -f "$(IDF_EXPORT)" ]; then source "$(IDF_EXPORT)" >/dev/null; fi; python3 $(ROOT)/tools/build/configure.py doctor --root "$(ROOT)" --compiler "$(CC)" --idf "$(dir $(IDF_EXPORT))"'
fetch: download toolchain-fetch btstack-source fetch-rootfs
build: resolved-config check-layout uboot linux rootfs radio-image
all: image
check: check-host check-docs check-dt
check-artifacts:
	python3 $(ROOT)/tools/release/manifest.py --output-root "$(BUILD_DIR)" --verify "$(IMAGES_DIR)/build-manifest.json"
radio-fs: radio-image
flash-image: image
initramfs: rootfs
bootloader: uboot
$(BUILD_DIR) $(IMAGES_DIR) $(GENERATED_DIR) $(STAGING_DIR) $(REPORTS_DIR) $(OPENSBI_OUT) $(LINUX_OUT) $(UBOOT_OUT) $(BUILDROOT_OUT) $(COREMARK_OUT) $(RADIO_OUT):
	mkdir -p $@

.PHONY: resolved-config
resolved-config: | $(REPORTS_DIR)
	python3 -c 'import json,pathlib; p=pathlib.Path("$(REPORTS_DIR)/resolved-config.json"); p.write_text(json.dumps(dict(debug="$(DEBUG)",kernel_fragments="$(KERNEL_FRAGMENTS)".split(),kernel_isa="$(S31_KERNEL_ISA)",kernel_flags="$(S31_KERNEL_FLAGS)",kernel_object_flags="$(S31_SIZE_OBJECT_FLAGS)".split(),optimization="-Os",userspace_abi="ilp32",radio_abi="ilp32f",toolchain="$(TOOLCHAIN_PREFIX)",idf="$(dir $(IDF_EXPORT))",jobs="$(JOBS)",source_date_epoch="$(SOURCE_DATE_EPOCH)",output_root="$(BUILD_DIR)",rootfs_baseline="$(ROOTFS_BASELINE)"),indent=2)+"\n")'
