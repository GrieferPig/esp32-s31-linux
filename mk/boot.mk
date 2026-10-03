# The FIT's fixed 0x400 external-data position places OpenSBI at the
# 64-byte-aligned NOR XIP address 0x4000e400.  Only writable state lives in
# coherent HP SRAM, leaving the retired loader area available to the radio.
FW_TEXT_START ?= 0x4000e400
FW_RW_START ?= 0x2F00F000
OPENSBI_MAX_SIZE ?= 262144

OPENSBI_FW_DYNAMIC_BIN := $(OPENSBI_OUT)/platform/generic/firmware/fw_dynamic.bin
OPENSBI_CONFIG_STAMP := $(OPENSBI_OUT)/.s31-link-config

opensbi: toolchain | $(OPENSBI_OUT)
	@echo "--- OpenSBI ---"
	@set -eu; \
	desired='FW_TEXT_START=$(FW_TEXT_START) FW_RW_START=$(FW_RW_START) ISA=$(S31_SAFE_ISA) OPT=-Os'; desired="$$desired override=$$(sha256sum $(ROOT)/mk/opensbi-size.mk | cut -d' ' -f1) compiler=$$(python3 $(ROOT)/tools/build/toolchain_identity.py --compiler $(CC))"; \
	actual=$$(cat "$(OPENSBI_CONFIG_STAMP)" 2>/dev/null || true); \
	if [ "$$actual" != "$$desired" ]; then \
		echo "OpenSBI link configuration changed; rebuilding its output tree"; \
	$(MAKE) -C $(OPENSBI_DIR) O=$(OPENSBI_OUT) clean; \
	mkdir -p "$(OPENSBI_OUT)"; \
	printf '%s\n' "$$desired" > "$(OPENSBI_CONFIG_STAMP)"; \
	fi
	$(MAKE) -C $(OPENSBI_DIR) -f Makefile -f $(ROOT)/mk/opensbi-size.mk O=$(OPENSBI_OUT) \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		PLATFORM=generic \
		PLATFORM_DEFCONFIG=esp32s31_defconfig \
		PLATFORM_RISCV_XLEN=32 \
		PLATFORM_RISCV_ISA=$(S31_SAFE_ISA) \
		FW_TEXT_START=$(FW_TEXT_START) \
		FW_RW_START=$(FW_RW_START) \
		FW_DYNAMIC=y V=$(V) \
		-j$(JOBS)
	@size=$$(stat -c%s $(OPENSBI_FW_DYNAMIC_BIN)); \
	if [ $$size -gt $(OPENSBI_MAX_SIZE) ]; then \
		echo "ERROR: OpenSBI fw_dynamic ($$size bytes) overlaps SPL at 0x2f040000"; exit 1; \
	fi
	python3 $(ROOT)/tools/checks/opensbi.py \
		--elf $(OPENSBI_OUT)/platform/generic/firmware/fw_dynamic.elf \
		--raw $(OPENSBI_FW_DYNAMIC_BIN)

uboot: idf-check opensbi | $(UBOOT_OUT) $(IMAGES_DIR)
	@echo "--- U-Boot SPL + proper ---"
	# Host Python is explicit; do not replace the host tool PATH with /usr/bin.
	python3 $(ROOT)/tools/build/configure.py native --kind uboot --source "$(UBOOT_DIR)" --output "$(UBOOT_OUT)" --defconfig espressif_esp32s31_defconfig --fragment "$(ROOT)/configs/uboot.config" --compiler "$(CC)" --cross "$(CROSS_COMPILE)"
	PYTHONPATH="$(UBOOT_PYTHONPATH)$${PYTHONPATH:+:$$PYTHONPATH}" $(MAKE) -C $(UBOOT_DIR) O=$(UBOOT_OUT) ARCH=riscv \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		PYTHON="$(HOST_PYTHON)" PYTHON3="$(HOST_PYTHON)" OPENSBI=$(OPENSBI_FW_DYNAMIC_BIN) -j$(JOBS)
	cp -v $(UBOOT_OUT)/u-boot.itb $(UBOOT_ITB)
	python3 $(ROOT)/tools/checks/layout.py --image-slot UBOOT_ITB "$(UBOOT_ITB)"
	python3 $(ROOT)/tools/checks/opensbi.py \
		--elf $(OPENSBI_OUT)/platform/generic/firmware/fw_dynamic.elf \
		--raw $(OPENSBI_FW_DYNAMIC_BIN) --fit $(UBOOT_ITB)
	cp -v $(UBOOT_OUT)/spl/u-boot-spl-dtb.bin $(UBOOT_SPL_DTB)
	@work=$$(mktemp -d "$(BUILD_DIR)/spl-wrap.XXXXXX"); \
	trap 'rm -rf "$$work"' EXIT; \
	$(CROSS_COMPILE)objcopy -I binary -O elf32-littleriscv -B riscv \
		$(UBOOT_SPL_DTB) "$$work/spl1.elf"; \
	$(CROSS_COMPILE)objcopy --change-section-address .data=0x2F040000 \
		--rename-section .data=.text,alloc,load,readonly,code,contents \
		--set-start 0x2F040000 "$$work/spl1.elf" "$$work/spl_wrapped.elf"; \
	bash -c "source $(IDF_EXPORT) >/dev/null && esptool --chip esp32s31 elf2image \
		--flash-mode dio --flash-freq 80m --flash-size 16MB \
		--output $(SPL_APP_BIN) $$work/spl_wrapped.elf"
	python3 $(ROOT)/tools/checks/layout.py --image-slot SPL "$(SPL_APP_BIN)"
