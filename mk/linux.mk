# Kbuild accepts per-object CFLAGS on its command line. These override existing
# local speed exceptions; LZ4's per-directory -O3 is superseded per object.
# The listed assignments contain optimization only in the pinned source tree.
S31_SIZE_OBJECTS := skbuff gro tcp tcp_input util rx aead_api wpa tx status sta_info esp32s31-radio-wifi lz4_compress lz4hc_compress lz4_decompress
S31_SIZE_OBJECT_FLAGS := $(foreach o,$(S31_SIZE_OBJECTS),CFLAGS_$(o).o=-Os)
LINUX_PARTITION_SIZE := $(shell python3 $(ROOT)/tools/checks/layout.py --size KERNEL)
.PHONY: linux
linux: check-layout toolchain radio-linux-payload | $(LINUX_OUT) $(IMAGES_DIR) $(REPORTS_DIR)
	python3 $(ROOT)/tools/build/configure.py native --kind linux --source "$(LINUX_DIR)" --output "$(LINUX_OUT)" --defconfig "$(DEFCONFIG)" --compiler "$(CC)" --cross "$(CROSS_COMPILE)" $(foreach f,$(KERNEL_FRAGMENTS),--fragment "$(f)") --cmdline "$(LINUX_CMDLINE)"
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" \
		S31_RADIO_IMPORTS="$(GENERATED_DIR)/esp32s31-radio-imports.S" \
		LOCALVERSION= KCFLAGS="-march=$(S31_KERNEL_ISA) $(S31_KERNEL_FLAGS)" \
		$(S31_SIZE_OBJECT_FLAGS) -j$(JOBS) $(LINUX_TARGET) modules dtbs
	cmp -s $(LINUX_OUT)/arch/riscv/boot/$(LINUX_TARGET) $(XIP_IMAGE) || cp $(LINUX_OUT)/arch/riscv/boot/$(LINUX_TARGET) $(XIP_IMAGE)
	cmp -s $(LINUX_OUT)/arch/riscv/boot/dts/espressif/esp32s31_generic.dtb $(FDT_DTB) || cp $(LINUX_OUT)/arch/riscv/boot/dts/espressif/esp32s31_generic.dtb $(FDT_DTB)
	cmp -s $(LINUX_OUT)/.config $(REPORTS_DIR)/linux.config || cp $(LINUX_OUT)/.config $(REPORTS_DIR)/linux.config
	python3 $(ROOT)/tools/checks/layout.py --image-slot KERNEL "$(XIP_IMAGE)"
