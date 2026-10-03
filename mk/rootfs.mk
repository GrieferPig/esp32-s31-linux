ROOTFS_PARTITION_SIZE := $(shell python3 $(ROOT)/tools/checks/layout.py --size ROOTFS)
PERSIST_PARTITION_SIZE := $(shell python3 $(ROOT)/tools/checks/layout.py --size PERSIST)
BUILDROOT_ENV = S31_KERNEL_CONFIG=$(LINUX_OUT)/.config S31_RADIO_MODULE=$(LINUX_OUT)/drivers/platform/esp32s31-radio.ko S31_GENERATED_DIR=$(GENERATED_DIR) S31_OVERLAY_DIR=$(STAGING_DIR)/overlay S31_BTSTACK_SOURCE=$(BTSTACK_SOURCE_DIR) S31_DTBO_DIR=$(LINUX_OUT)/arch/riscv/boot/dts/espressif
BUILDROOT_MAKE = $(BUILDROOT_ENV) $(MAKE) -C $(BUILDROOT_DIR) O=$(BUILDROOT_OUT) BR2_EXTERNAL=$(BUILDROOT_EXTERNAL) BR2_DL_DIR=$(BUILDROOT_DL_DIR) BR2_JLEVEL=$(JOBS) BR2_WGET=false BR2_CURL=false BR2_GIT=false BR2_SVN=false BR2_HG=false
.PHONY: rootfs s31-pie-cases btstack-source btstack-notices lp-firmware coremark
s31-pie-cases: idf-check | $(GENERATED_DIR)
	bash -c 'source "$(IDF_EXPORT)" >/dev/null && $(ROOT)/rootfs/gen_s31_pie_cases.sh $(GENERATED_DIR)/s31_pie_cases.inc'
btstack-source:
	S31_CACHE_ROOT=$(CACHE_DIR) $(ROOT)/tools/build/fetch_btstack.sh $(BTSTACK_SOURCE_DIR)
btstack-notices:
	@test -f $(BTSTACK_SOURCE_DIR)/LICENSE && test "$$(cat $(BTSTACK_SOURCE_DIR)/.s31-btstack-version 2>/dev/null)" = "$(BTSTACK_REF)" || { echo 'Missing or wrong-version BTstack sources: run make fetch' >&2; exit 1; }
	S31_OUTPUT_ROOT=$(BUILD_DIR) $(ROOT)/tools/release/btstack_notices.sh $(BTSTACK_SOURCE_DIR) $(IMAGES_DIR)/btstack-s31-notices.tar.xz
lp-firmware: idf-check | $(STAGING_DIR)
	bash -c 'source "$(IDF_EXPORT)" >/dev/null && $(MAKE) -C "$(ROOT)/firmware/lp" O="$(LP_OUT)" IDF_PATH="$$IDF_PATH" STAGE_DIR="$(STAGING_DIR)/overlay/lib/firmware/esp32s31" stage'
ifneq ($(strip $(ROOTFS_BASELINE)),)
rootfs: linux | $(IMAGES_DIR) $(REPORTS_DIR) $(STAGING_DIR)
	python3 $(ROOT)/tools/build/rootfs_baseline.py --baseline "$(ROOTFS_BASELINE)" --kernel-output "$(LINUX_OUT)" $(if $(strip $(ROOTFS_BUSYBOX_BUILD)),--restore-full-services-from "$(BUILDROOT_DIR)" --busybox-build "$(ROOTFS_BUSYBOX_BUILD)") --module $(LINUX_OUT)/drivers/platform/esp32s31-radio.ko --strip $(CROSS_COMPILE)strip --output $(ROOTFS_IMG) --report $(REPORTS_DIR)/rootfs-provenance.json --staging $(STAGING_DIR) --jobs $(JOBS)
	python3 $(ROOT)/tools/checks/layout.py --image-slot ROOTFS "$(ROOTFS_IMG)"
else
rootfs: linux toolchain s31-pie-cases lp-firmware | $(BUILDROOT_OUT) $(IMAGES_DIR) $(REPORTS_DIR)
	@test -f $(BTSTACK_SOURCE_DIR)/LICENSE && test "$$(cat $(BTSTACK_SOURCE_DIR)/.s31-btstack-version 2>/dev/null)" = "$(BTSTACK_REF)" || { echo 'Missing or wrong-version BTstack sources: run make fetch' >&2; exit 1; }
	python3 $(ROOT)/tools/build/configure.py buildroot --source $(BUILDROOT_DIR) --output $(BUILDROOT_OUT) --source-config $(BUILDROOT_EXTERNAL)/configs/esp32s31_rootfs_defconfig --external $(BUILDROOT_EXTERNAL) --compiler $(CC) --toolchain $(TOOLCHAIN_PREFIX) --static-overlay $(BUILDROOT_EXTERNAL)/board/esp32-s31/overlay --overlay $(STAGING_DIR)/overlay
	$(BUILDROOT_ENV) python3 $(ROOT)/tools/build/configure.py package-inputs --downloads $(BUILDROOT_DL_DIR) --jobs $(JOBS) --source $(BUILDROOT_DIR) --output $(BUILDROOT_OUT) --external $(BUILDROOT_EXTERNAL) --package esp-simd --stamp $(BUILDROOT_OUT)/.esp-simd-inputs.json --input $(BUILDROOT_EXTERNAL)/package/esp-simd --input $(ROOT)/rootfs/esp_simd.c --input $(ROOT)/rootfs/esp_simd.h --input $(ROOT)/rootfs/s31_xespv_memops.S
	$(BUILDROOT_ENV) python3 $(ROOT)/tools/build/configure.py package-inputs --downloads $(BUILDROOT_DL_DIR) --jobs $(JOBS) --source $(BUILDROOT_DIR) --output $(BUILDROOT_OUT) --external $(BUILDROOT_EXTERNAL) --package s31-tools --stamp $(BUILDROOT_OUT)/.s31-tools-inputs.json --input $(ROOT)/rootfs --input $(BUILDROOT_EXTERNAL)/package/s31-tools --input $(GENERATED_DIR)/s31_pie_cases.inc --input $(LINUX_DIR)/include/uapi/linux/esp32s31-overlay.h
	$(BUILDROOT_ENV) python3 $(ROOT)/tools/build/configure.py package-inputs --downloads $(BUILDROOT_DL_DIR) --jobs $(JOBS) --source $(BUILDROOT_DIR) --output $(BUILDROOT_OUT) --external $(BUILDROOT_EXTERNAL) --package btstack-s31 --stamp $(BUILDROOT_OUT)/.btstack-inputs.json --input $(BUILDROOT_EXTERNAL)/package/btstack-s31 --value $(BTSTACK_REF) --input $(BTSTACK_SOURCE_DIR)
	$(BUILDROOT_ENV) python3 $(ROOT)/tools/build/configure.py package-inputs --downloads $(BUILDROOT_DL_DIR) --jobs $(JOBS) --source $(BUILDROOT_DIR) --output $(BUILDROOT_OUT) --external $(BUILDROOT_EXTERNAL) --package coremark --stamp $(BUILDROOT_OUT)/.coremark-inputs.json --input $(BUILDROOT_EXTERNAL)/package/coremark
	$(BUILDROOT_MAKE)
	cmp -s $(BUILDROOT_OUT)/images/rootfs.squashfs $(ROOTFS_IMG) || cp $(BUILDROOT_OUT)/images/rootfs.squashfs $(ROOTFS_IMG)
	python3 $(ROOT)/tools/checks/layout.py --image-slot ROOTFS "$(ROOTFS_IMG)"
	cp $(BUILDROOT_OUT)/.config $(REPORTS_DIR)/buildroot.config
	python3 -c 'import json,hashlib,pathlib; h=lambda p:hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest(); pathlib.Path("$(REPORTS_DIR)/rootfs-provenance.json").write_text(json.dumps(dict(mode="native-buildroot",module_sha256=h("$(LINUX_OUT)/drivers/platform/esp32s31-radio.ko"),rootfs_sha256=h("$(ROOTFS_IMG)"),packaged_module_sha256=h("$(BUILDROOT_OUT)/target/usr/lib/s31-radio/esp32s31-radio.ko.xz")),indent=2)+"\n")'
endif
coremark: rootfs | $(COREMARK_OUT)
	@set -- $(BUILDROOT_OUT)/build/coremark-*/coremark; test -x "$$1"; cp "$$1" "$(COREMARK_BIN)"

.PHONY: fetch-rootfs
fetch-rootfs: toolchain | $(BUILDROOT_OUT) $(STAGING_DIR)
	mkdir -p $(STAGING_DIR)/overlay
	python3 $(ROOT)/tools/build/configure.py buildroot --source $(BUILDROOT_DIR) --output $(BUILDROOT_OUT) --source-config $(BUILDROOT_EXTERNAL)/configs/esp32s31_rootfs_defconfig --external $(BUILDROOT_EXTERNAL) --compiler $(CC) --toolchain $(TOOLCHAIN_PREFIX) --static-overlay $(BUILDROOT_EXTERNAL)/board/esp32-s31/overlay --overlay $(STAGING_DIR)/overlay
	$(BUILDROOT_ENV) $(MAKE) -C $(BUILDROOT_DIR) O=$(BUILDROOT_OUT) BR2_EXTERNAL=$(BUILDROOT_EXTERNAL) BR2_DL_DIR=$(BUILDROOT_DL_DIR) BR2_JLEVEL=$(JOBS) source
