# The IDF closure precedes relocatable payload, then Linux precedes final XIP.
S31_RADIO_KSYMS := $(GENERATED_DIR)/radio-kernel-symbols.txt
.PHONY: idf-check radio-idf-deps radio-linux-payload radio-module radio-image radio-package
idf-check:
	@test -f "$(IDF_EXPORT)" || { echo "ESP-IDF export.sh missing; set IDF_EXPORT in local.mk" >&2; exit 1; }
	python3 $(ROOT)/tools/checks/versions.py --idf "$(dir $(IDF_EXPORT))" $(if $(filter 1,$(S31_ALLOW_UNPINNED)),--allow-unpinned)
radio-idf-deps: idf-check | $(GENERATED_DIR)
	bash -c 'source "$(IDF_EXPORT)" >/dev/null && python3 "$(ROOT)/tools/build/configure.py" idf --source "$(RADIO_IDF_DEPS_DIR)" --output "$(RADIO_IDF_BUILD)" --jobs "$(JOBS)" --link-list "$(ROOT)/firmware/radio/boot_link.txt"'
radio-linux-payload: radio-idf-deps | $(RADIO_OUT) $(GENERATED_DIR)
	$(MAKE) -C $(RADIO_OUT) -f $(ROOT)/firmware/radio/Makefile O=$(RADIO_OUT) IDF_ROOT="$(IDF_ROOT)" IDF_EXPORT="$(IDF_EXPORT)" \
		IDF_DEPS_DIR="$(RADIO_IDF_DEPS_DIR)" RADIO_BUILD="$(RADIO_IDF_BUILD)" \
		LINUX_RADIO_FW="$(GENERATED_DIR)/esp32s31-radio-fw-v1.o" LINUX_RADIO_IMPORTS="$(GENERATED_DIR)/esp32s31-radio-imports.S" \
		linux-kbuild
	python3 $(ROOT)/tools/build/configure.py radio-exports --nm "$(CROSS_COMPILE)nm" --payload "$(RADIO_OUT)/linux_radio.localized.o" --output "$(S31_RADIO_KSYMS)"
radio-module: linux
	@test -f "$(LINUX_OUT)/drivers/platform/esp32s31-radio.ko"
	@test -f "$(GENERATED_DIR)/esp32s31-radio-fw-v1.o"
radio-image: linux | $(IMAGES_DIR)
	python3 $(ROOT)/tools/build/radio_image.py \
		--prefix $(CROSS_COMPILE) --kernel $(LINUX_OUT)/vmlinux \
		--module $(LINUX_OUT)/drivers/platform/esp32s31-radio.ko \
		--payload $(RADIO_OUT)/linux_radio.localized.o \
		--imports $(RADIO_OUT)/linux-radio-linked-imports.txt --output $(RADIO_IMAGE)
radio-package:
	S31_OUTPUT_ROOT=$(BUILD_DIR) S31_LINUX_OUT=$(LINUX_OUT) CROSS_COMPILE=$(CROSS_COMPILE) $(ROOT)/tools/release/radio_bundle.sh
