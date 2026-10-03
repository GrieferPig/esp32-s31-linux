# SPDX-License-Identifier: BSD-2-Clause
SHELL := /bin/bash
.DEFAULT_GOAL := all

LP_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
LP_SRC := $(patsubst %/,%,$(dir $(LP_MAKEFILE)))
PROJECT_DIR := $(abspath $(LP_SRC)/../..)
include $(PROJECT_DIR)/configs/build-versions.mk
ifneq ($(origin PROFILE),undefined)
$(error PROFILE is removed; use the full board build)
endif
O ?= $(PROJECT_DIR)/out/lp
BUILD_DIR := $(abspath $(O))
IDF_PATH ?= $(HOME)/.espressif/master/esp-idf
IDF_PY := $(IDF_PATH)/tools/idf.py
ESP_TOOLS ?= $(if $(IDF_TOOLS_PATH),$(IDF_TOOLS_PATH),$(HOME)/.espressif)/tools/riscv32-esp-elf/$(ESP_ELF_RELEASE)/riscv32-esp-elf
LP_CC ?= $(ESP_TOOLS)/bin/riscv32-esp-elf-gcc
LP_OUTPUT := $(BUILD_DIR)/esp-idf/main/s31_lp_main/s31_lp_main
STAGE_DIR ?= $(abspath $(BUILD_DIR)/../staging/overlay/lib/firmware/esp32s31)
SDKCONFIG := $(BUILD_DIR)/sdkconfig

# Reject an accidental source-tree output rather than silently polluting it.
ifeq ($(BUILD_DIR),$(LP_SRC))
$(error LP O must name a separate output directory)
endif

.PHONY: all stage clean FORCE
FORCE:

$(BUILD_DIR):
	mkdir -p "$@"

# A changed defaults/IDF identity needs a fresh SDKCONFIG; an unchanged
# invocation leaves it intact and lets IDF/Ninja perform its normal no-op.
$(BUILD_DIR)/.s31-config-inputs: FORCE | $(BUILD_DIR)
	python3 "$(PROJECT_DIR)/tools/build/input_stamp.py" --output "$@" \
		--repo "$(IDF_PATH)" --file "$(LP_SRC)/sdkconfig.defaults" \
		--file "$(PROJECT_DIR)/configs/build-versions.mk" \
		--value="$(IDF_PATH) $(BUILD_DIR)"

$(BUILD_DIR)/.s31-toolchain-inputs: FORCE | $(BUILD_DIR)
	python3 "$(PROJECT_DIR)/tools/build/input_stamp.py" --output "$@" \
		--compiler "$(LP_CC)"

all: $(BUILD_DIR)/.s31-config-inputs $(BUILD_DIR)/.s31-toolchain-inputs
	@. "$(IDF_PATH)/export.sh" >/dev/null && \
	if [ "$(BUILD_DIR)/.s31-toolchain-inputs" -nt "$(BUILD_DIR)/.s31-toolchain-applied" ]; then \
		for output in "$(BUILD_DIR)/esp-idf/main/s31_lp_main" "$(BUILD_DIR)/bootloader" "$(BUILD_DIR)"; do \
			if [ -f "$$output/build.ninja" ]; then ninja -C "$$output" clean || exit $$?; fi; \
		done; \
		rm -f "$(SDKCONFIG)" "$(SDKCONFIG).old"; \
	fi
	@if [ "$(BUILD_DIR)/.s31-config-inputs" -nt "$(SDKCONFIG)" ]; then \
		rm -f "$(SDKCONFIG)" "$(SDKCONFIG).old"; \
	fi
	. "$(IDF_PATH)/export.sh" >/dev/null && \
		IDF_TARGET=esp32s31 "$(IDF_PY)" -C "$(LP_SRC)" -B "$(BUILD_DIR)" \
		-D "SDKCONFIG=$(SDKCONFIG)" \
		-D "SDKCONFIG_DEFAULTS=$(LP_SRC)/sdkconfig.defaults" build
	@cmp -s "$(BUILD_DIR)/.s31-toolchain-inputs" "$(BUILD_DIR)/.s31-toolchain-applied" || \
		cp "$(BUILD_DIR)/.s31-toolchain-inputs" "$(BUILD_DIR)/.s31-toolchain-applied"

stage: all
	@mkdir -p "$(STAGE_DIR)"
	@cmp -s "$(LP_OUTPUT).elf" "$(STAGE_DIR)/s31-lp-core.elf" || \
		install -m 0644 "$(LP_OUTPUT).elf" "$(STAGE_DIR)/s31-lp-core.elf"
	@cmp -s "$(LP_OUTPUT).bin" "$(STAGE_DIR)/s31-lp-core.bin" || \
		install -m 0644 "$(LP_OUTPUT).bin" "$(STAGE_DIR)/s31-lp-core.bin"

clean:
	@test "$(BUILD_DIR)" != / && test "$(BUILD_DIR)" != "$(PROJECT_DIR)"
	rm -rf "$(BUILD_DIR)"
