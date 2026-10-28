# SPDX-License-Identifier: BSD-2-Clause
# ESP32-S31 Linux S-mode Wi-Fi/Bluetooth payload.
# Source paths come from this file, never the invocation working directory.
RADIO_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
RADIO_SRC := $(patsubst %/,%,$(dir $(RADIO_MAKEFILE)))
PROJECT_DIR := $(abspath $(RADIO_SRC)/../..)
include $(PROJECT_DIR)/configs/build-versions.mk

O ?= $(PROJECT_DIR)/out/radio
RADIO_OUT := $(abspath $(O))
GENERATED_DIR ?= $(abspath $(RADIO_OUT)/../generated)
ifneq ($(filter $(RADIO_OUT),$(RADIO_SRC) $(PROJECT_DIR)),)
$(error Radio O must name a separate output directory)
endif

# Direct component invocations are also out-of-tree. The facade normally
# enters RADIO_OUT itself and supplies this Makefile with -f.
ifneq ($(CURDIR),$(RADIO_OUT))
.DEFAULT_GOAL := all
.PHONY: all clean idf-check linux-radio linux-kbuild
all clean idf-check linux-radio linux-kbuild:
	+@mkdir -p "$(RADIO_OUT)"; $(MAKE) -C "$(RADIO_OUT)" -f "$(RADIO_MAKEFILE)" O="$(RADIO_OUT)" $@
else
.DEFAULT_GOAL := all

IDF_DEPS_DIR ?= $(RADIO_SRC)/idf_deps
IDF_DEPS_ABS := $(abspath $(IDF_DEPS_DIR))
RADIO_BUILD  ?= $(abspath $(RADIO_OUT)/../idf-radio)
RADIO_BUILD_ABS := $(abspath $(RADIO_BUILD))
RADIO_SDKCONFIG := $(RADIO_BUILD_ABS)/config/sdkconfig.h
IDF_ROOT     ?= $(HOME)/.espressif
IDF_EXPORT   ?= $(IDF_ROOT)/master/esp-idf/export.sh
IDF_PATH     ?= $(patsubst %/export.sh,%,$(IDF_EXPORT))
# Match the compiler declared by the pinned IDF tools manifest.
ESP_TOOLS ?= $(IDF_ROOT)/tools/riscv32-esp-elf/$(ESP_ELF_RELEASE)/riscv32-esp-elf

# Resolve the ILP32F multilib using the pinned compiler. GCC16 renamed its
# ISA multilib directories; do not silently substitute a different ABI.
RADIO_ISA := rv32imafc_zicsr_zifencei_zaamo_zalrsc
PICOLIBC_MULTI := $(shell "$(ESP_TOOLS)/bin/riscv32-esp-elf-gcc" -march=$(RADIO_ISA) -mabi=ilp32f -print-multi-directory)
PICOLIBC_LIB := $(ESP_TOOLS)/picolibc/riscv32-esp-elf/lib/$(PICOLIBC_MULTI)
PICOLIBC_GCC := $(dir $(shell "$(ESP_TOOLS)/bin/riscv32-esp-elf-gcc" -march=$(RADIO_ISA) -mabi=ilp32f -print-libgcc-file-name))

# The component closure is captured from IDF's generated build.  Its path
# placeholders are resolved at build time, so the capture has no developer-
# specific absolute paths.
IDF_CAPTURED_INCLUDES := $(shell sed -e 's|@IDF_PATH@|$(IDF_PATH)|g' -e 's|@RADIO_BUILD@|$(RADIO_BUILD_ABS)|g' $(RADIO_SRC)/idf_includes.rsp)
IDF_BOOT_LINKPATH := $(shell sed -e 's|@IDF_PATH@|$(IDF_PATH)|g' -e 's|@RADIO_BUILD@|$(RADIO_BUILD_ABS)|g' $(RADIO_SRC)/boot_linkpath.txt)
IDF_NET80211 := $(IDF_PATH)/components/esp_wifi/lib/esp32s31/libnet80211.a
LINUX_NET80211 := libnet80211.s31-linux.a
IDF_PP := $(IDF_PATH)/components/esp_wifi/lib/esp32s31/libpp.a
# Resolve one ordered archive set for both the linker and dependency graph.
# radio-idf-deps must finish before entering this component Makefile.
RADIO_ARCHIVES := $(shell python3 $(PROJECT_DIR)/tools/build/radio_archives.py $(RADIO_SRC)/boot_link.txt "$(IDF_PATH)" "$(RADIO_BUILD_ABS)")
LINUX_NOFREERTOS_LINK := $(subst $(IDF_NET80211),$(LINUX_NET80211),$(RADIO_ARCHIVES))

HEAP_WRAP_APIS := heap_caps_malloc heap_caps_free heap_caps_calloc \
	heap_caps_realloc heap_caps_aligned_alloc heap_caps_aligned_calloc \
	heap_caps_malloc_default heap_caps_realloc_default \
	heap_caps_malloc_prefer heap_caps_get_free_size \
	heap_caps_get_largest_free_block heap_caps_add_region
comma := ,
HEAP_WRAP_FLAGS := $(addprefix -Wl$(comma)--wrap=,$(HEAP_WRAP_APIS))
LINUX_WRAP_APIS := r_orca_lmp_tbl_register r_os_memblock_put r_os_memblock_put_from_cb time esp_log_timestamp esp_event_loop_create esp_timer_create esp_timer_delete \
	esp_timer_start_once esp_timer_start_once_at esp_timer_start_periodic \
	esp_timer_start_periodic_at esp_timer_stop esp_timer_restart \
	ets_timer_done os_timer_done \
	ets_timer_arm ets_timer_arm_us os_timer_arm os_timer_arm_us \
	esp_timer_is_active esp_timer_get_time esp_intr_ptr_in_isr_region \
	esp_intr_alloc esp_intr_enable esp_intr_disable esp_intr_free \
	esp_ipc_call_blocking \
	mpi_hal_write_to_mem_block mpi_hal_write_at_offset mpi_hal_write_rinv \
	mpi_hal_read_result_hw_op \
	   \
	    \
	coex_enable coex_disable \
	coex_register_start_cb  \
	  \

CRYPTO_WRAP_APIS := $(addprefix esp_crypto_,$(addsuffix _lock_acquire,sha_aes dma mpi ecc ecdsa hmac ds key_manager)) \
	$(addprefix esp_crypto_,$(addsuffix _lock_release,sha_aes dma mpi ecc ecdsa hmac ds key_manager)) \
	$(addprefix esp_crypto_,$(addsuffix _enable_periph_clk,aes sha mpi ecc ecdsa hmac ds key_mgr)) \
	esp_crypto_common_clk_enable
LINUX_WRAP_APIS += $(CRYPTO_WRAP_APIS)
LINUX_WRAP_FLAGS := $(addprefix -Wl$(comma)--wrap=,$(LINUX_WRAP_APIS))

.PHONY: all clean idf-check linux-radio linux-kbuild

all: linux-radio

idf-check:
	python3 $(PROJECT_DIR)/tools/checks/versions.py --idf "$(IDF_PATH)" $(if $(filter 1,$(S31_ALLOW_UNPINNED)),--allow-unpinned)
	@test -f "$(IDF_PATH)/export.sh" || { echo "ERROR: ESP-IDF export.sh not found under $(IDF_ROOT)" >&2; exit 1; }
	@test -x "$(ESP_TOOLS)/bin/riscv32-esp-elf-gcc" || { echo "ERROR: ESP-IDF RISC-V toolchain with picolibc not found under $(IDF_ROOT)/tools" >&2; exit 1; }
	@test -d "$(PICOLIBC_LIB)" && test -d "$(PICOLIBC_GCC)" || { echo "ERROR: required ILP32F picolibc multilib is missing" >&2; exit 1; }
	@test -f "$(RADIO_BUILD)/build.ninja" || { echo "ERROR: missing radio-only IDF build at $(RADIO_BUILD)" >&2; exit 1; }

# Relocatable payload used by the Linux S-mode driver. It intentionally
# contains no OpenSBI objects and performs no M-mode setup.
# The final kernel integration prefixes its private IDF namespace and supplies
# the small set of remaining Linux OS-adapter entry points.
linux-radio: idf-check linux_radio.resolved.o linux_radio.kbuild.o linux-radio-undefined.txt

LINUX_RADIO_FW ?= $(RADIO_OUT)/esp32s31-radio-fw-v1.o
LINUX_RADIO_IMPORTS ?= $(GENERATED_DIR)/esp32s31-radio-imports.S
LINUX_RADIO_LINKED_IMPORTS ?= $(RADIO_SRC)/linux-radio-linked-imports.txt
RADIO_COEX_PRINTF_OBJ := s31_coex_printf.o

linux-kbuild: idf-check linux_radio.kbuild.o linux-radio-linked-imports.txt
	mkdir -p $(dir $(LINUX_RADIO_FW)) $(dir $(LINUX_RADIO_IMPORTS))
	@cmp -s linux_radio.kbuild.o $(LINUX_RADIO_FW) || cp -f linux_radio.kbuild.o $(LINUX_RADIO_FW)
	python3 $(PROJECT_DIR)/tools/build/radio_imports.py \
		--nm $(ESP_TOOLS)/bin/riscv32-esp-elf-nm \
		--linked-symbols $(LINUX_RADIO_LINKED_IMPORTS) \
		linux_radio.kbuild.o $(LINUX_RADIO_IMPORTS)

# Keep the exact allowlist beside the payload for the later XIP packaging step.
linux-radio-linked-imports.txt: $(LINUX_RADIO_LINKED_IMPORTS)
	@cmp -s "$<" "$@" || cp -f "$<" "$@"

$(LINUX_NET80211): $(IDF_NET80211)
	cp $< $@

linux_radio.raw.o: radio_stack.linux.o s31_pp_submit.o s31_vhci.o s31_vhci_pool_wake.o nofreertos_link.txt $(RADIO_SRC)/boot_flags.txt $(RADIO_SRC)/boot_linkpath.txt $(LINUX_NET80211) $(S31RTOS_LINUX_OBJS) $(RADIO_COEX_PRINTF_OBJ) $(RADIO_MAKEFILE) $(PROJECT_DIR)/tools/build/radio_tls.py $(RADIO_SRC)/prepare_xip_sections.py
	$(ESP_CC) -nostdlib -r -Wl,--gc-sections -Wl,--unique=.iram1* -Wl,--unique=.wifi*iram* $(HEAP_WRAP_FLAGS) $(LINUX_WRAP_FLAGS) \
		-Wl,--undefined=s31_radio_stack_task \
		-Wl,--undefined=s31_radio_wifi_control_task \
		-Wl,--undefined=s31_radio_wifi_try_send_interface \
		-Wl,--undefined=s31_radio_wifi_scan_task \
		-Wl,--undefined=s31_radio_wifi_connect_task \
		-Wl,--undefined=s31_radio_wifi_disconnect_task \
		-Wl,--undefined=s31_radio_wifi_read_mac \
		-Wl,--undefined=s31_radio_wifi_clock_enable \
		-Wl,--undefined=s31_radio_wifi_clock_disable \
		-Wl,--undefined=s31_radio_wifi_try_send \
		-Wl,--undefined=s31_radio_coex_status \
		-Wl,--undefined=s31_radio_bt_enable_task \
		-Wl,--undefined=s31_radio_bt_disable_task \
		-Wl,--undefined=s31_radio_shutdown_task \
		-Wl,--undefined=s31_radio_vhci_try_send \
		-Wl,--undefined=s31_radio_fw_abi_version \
		-Wl,--undefined=s31_rtos_init \
		-Wl,--undefined=s31_rtos_tick \
		-Wl,--undefined=s31_linux_timer_next_deadline_ns \
		-Wl,--undefined=s31_rtos_hard_tick \
		-Wl,--undefined=s31_rtos_free \
		-Wl,--undefined=s31_rtos_task_release \
		-Wl,--undefined=s31_rtos_isr_depth \
		-Wl,--undefined=xTaskCreatePinnedToCore \
		$(shell cat $(RADIO_SRC)/boot_flags.txt) \
		$(IDF_BOOT_LINKPATH) \
		radio_stack.linux.o s31_pp_submit.o s31_vhci.o s31_vhci_pool_wake.o $(S31RTOS_LINUX_OBJS) $(RADIO_COEX_PRINTF_OBJ) \
		$(LINUX_NOFREERTOS_LINK) \
		-L$(PICOLIBC_LIB) -L$(PICOLIBC_GCC) -lc -lm -lgcc -o $@
	python3 $(PROJECT_DIR)/tools/build/radio_tls.py $@
	python3 $(RADIO_SRC)/prepare_xip_sections.py $@

linux_radio.exposed.o: linux_radio.raw.o $(RADIO_MAKEFILE)
	$(ESP_TOOLS)/bin/riscv32-esp-elf-objcopy \
		--globalize-symbol=wifi_on_coex_start_process \
		--globalize-symbol=wifi_on_coex_schm_phase_process $< $@

# Preserve the Wi-Fi owner's IPC endpoints. Native Linux owners remain
# independently scheduled; this does not add a global execution gate.
# SoftMAC publishes coexistence state through the existing scheduler APIs.
linux_radio.resolved.o: linux_radio.exposed.o $(PROJECT_DIR)/tools/build/radio_crypto_adapters.py
	$(ESP_TOOLS)/bin/riscv32-esp-elf-ld -r --no-relax $< -o $@
	python3 $(PROJECT_DIR)/tools/build/radio_crypto_adapters.py --audit $@ --objdump $(ESP_TOOLS)/bin/riscv32-esp-elf-objdump

linux-radio-undefined.txt: linux_radio.resolved.o
	$(ESP_TOOLS)/bin/riscv32-esp-elf-nm -u $< | sort -u > $@

linux-radio-localize.txt: linux_radio.resolved.o $(RADIO_MAKEFILE)
	$(ESP_TOOLS)/bin/riscv32-esp-elf-nm -g --defined-only $< | \
		awk '{ n=$$NF; if (n !~ /^(s31_radio_fw_abi_version|s31_radio_(stack|shutdown|bt_(enable|disable)|wifi_(scan|connect|disconnect|control))_task|s31_radio_wifi_(read_mac|clock_enable|clock_disable)|s31_radio_(vhci|wifi)_try_send|s31_radio_wifi_try_send_interface|s31_radio_coex_status|s31_rtos_(init|tick|hard_tick|free|task_release|isr_depth)|s31_linux_timer_next_deadline_ns|xTaskCreatePinnedToCore)$$/) print n; }' | \
		sort -u > $@

linux_radio.localized.o: linux_radio.resolved.o linux-radio-localize.txt
	$(ESP_TOOLS)/bin/riscv32-esp-elf-objcopy \
		--strip-debug \
		--localize-symbols=linux-radio-localize.txt $< $@

linux_radio.kbuild.o: linux_radio.localized.o $(RADIO_SRC)/linux_module_sections.lds
	$(ESP_TOOLS)/bin/riscv32-esp-elf-ld -r --no-relax \
		-T $(RADIO_SRC)/linux_module_sections.lds $< -o $@

S31RTOS_LINUX_OBJS := s31_rtos/s31_rtos_core.linux.o \
		s31_rtos/s31_rtos_queue.o s31_rtos/s31_rtos_event.o \
		s31_linux_locks.o s31_linux_timer.o s31_memcpy.o s31_linux_crypto.o \
		s31_sdk_ecc.o s31_sdk_mpi.o s31_sdk_ecdsa.o s31_sdk_hmac.o s31_sdk_tinycrypt.o
# The main rule appears above this simply-expanded list; add the prerequisites
# here so a clean/forced build cannot reach the linker before they exist.
linux_radio.raw.o: $(S31RTOS_LINUX_OBJS)
s31_coex_printf.o: $(IDF_PATH)/components/esp_coex/src/lib_printf.c
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -c $< -o $@
s31_rtos/%.o: $(RADIO_SRC)/s31_rtos/%.c $(RADIO_SRC)/s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -I $(RADIO_SRC)/s31_rtos -c $< -o $@

s31_rtos/s31_rtos_core.linux.o: $(RADIO_SRC)/s31_rtos/s31_rtos_core.c $(RADIO_SRC)/s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -DS31_LINUX_SMODE -I $(RADIO_SRC)/s31_rtos -c $< -o $@

# Keep a readable record of precisely the archives linked into the payload.
nofreertos_link.txt: $(RADIO_SRC)/boot_link.txt $(RADIO_BUILD_ABS)/build.ninja .s31-build-config
	@printf '%s\n' $(LINUX_NOFREERTOS_LINK) > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@

ESP_CC      := $(ESP_TOOLS)/bin/riscv32-esp-elf-gcc
s31_pp_submit.S: $(IDF_PP) $(PROJECT_DIR)/tools/build/radio_pp_submit.py
	python3 $(PROJECT_DIR)/tools/build/radio_pp_submit.py \
		--ar $(ESP_TOOLS)/bin/riscv32-esp-elf-ar --archive $(IDF_PP) --output $@
s31_pp_submit.o: s31_pp_submit.S $(RADIO_MAKEFILE)
	$(ESP_CC) $(ESP_CFLAGS) -c $< -o $@
ifneq ($(origin S31_WIFI_ONLY),undefined)
$(error S31_WIFI_ONLY is removed; every build includes Wi-Fi and Bluetooth)
endif
RADIO_STACK_DEFS := -DS31_LINUX_SMODE
ESP_CFLAGS  := -Os -mabi=ilp32f -march=$(RADIO_ISA) \
		-mtune=esp-base -ffunction-sections -fdata-sections \
		-fno-stack-protector -fno-builtin -ffreestanding -std=gnu11 -Wall \
	-specs=picolibc.specs \
		-I$(IDF_PATH)/components/bt/include/esp32s31/include \
		-I$(IDF_PATH)/components/bt/porting_btdm/controller/btdm_common/include \
		-I$(IDF_PATH)/components/bt/porting_btdm/controller/ble/include \
		-I$(IDF_PATH)/components/bt/porting_btdm/controller/bredr/include \
		-I$(IDF_PATH)/components/bt/porting_btdm/transport/include \
		-I$(IDF_PATH)/components/esp_hal_timg/include \
		-I$(IDF_PATH)/components/esp_hal_timg/esp32s31/include \
		$(IDF_CAPTURED_INCLUDES)
# radio_stack.c expands BT_CONTROLLER_INIT_CONFIG_DEFAULT(), so its object must
# be rebuilt whenever the IDF configuration changes.  Without this dependency,
# btdm_lp.c could be rebuilt with controller sleep enabled while this object
# retained sleep_en=0, leaving the closed controller's sleep environment NULL.
radio_stack.linux.o: $(addprefix $(RADIO_SRC)/,radio_stack.c s31_wifi_rom_runtime.inc s31_softmac.inc s31_softmac_coex_pm.inc s31_softmac_tx_agg.inc s31_softmac_tx_reports.inc s31_rtos/s31_rtos.h) $(PROJECT_DIR)/linux-esp32-s31/include/linux/esp32s31-radio-control.h $(RADIO_MAKEFILE) $(RADIO_SDKCONFIG)
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP $(RADIO_STACK_DEFS) \
		-I $(RADIO_SRC)/s31_rtos -c $< -o $@

s31_vhci.o: $(RADIO_SRC)/s31_vhci.c $(RADIO_SRC)/s31_vhci_command.inc $(RADIO_SRC)/s31_vhci_acl.inc $(RADIO_SRC)/s31_vhci_bredr.inc $(IDF_PATH)/components/bt/porting_btdm/transport/driver/vhci/hci_driver_standard.c $(RADIO_SDKCONFIG)
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP \
		-I$(IDF_PATH)/components/bt/porting_btdm/transport/driver/vhci -c $< -o $@

s31_vhci_pool_wake.o: $(RADIO_SRC)/s31_vhci_pool_wake.c $(RADIO_SRC)/s31_vhci_pool_wake.inc
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -c $< -o $@

s31_linux_locks.o: $(RADIO_SRC)/s31_linux_locks.c $(RADIO_SRC)/s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -DS31_LINUX_SMODE -I $(RADIO_SRC)/s31_rtos -c $< -o $@

s31_linux_timer.o: $(RADIO_SRC)/s31_linux_timer.c $(RADIO_SRC)/s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -DS31_LINUX_SMODE -I $(RADIO_SRC)/s31_rtos -c $< -o $@

s31_linux_crypto.o: $(RADIO_SRC)/s31_linux_crypto.c $(RADIO_SRC)/s31_sdk_crypto.h $(PROJECT_DIR)/shared/s31_crypto.h
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -c $< -o $@

# Keep checked SDK adapters outside the vendor checkout. Source hashes in the
# generator reject an SDK update until its error paths have been audited.
S31_SDK_CRYPTO_SOURCES := $(IDF_PATH)/components/mbedtls/port/ecc/esp_ecc.c \
	$(IDF_PATH)/components/mbedtls/port/bignum/esp_bignum.c \
	$(IDF_PATH)/components/mbedtls/port/psa_driver/esp_ecdsa/psa_crypto_driver_esp_ecdsa.c \
	$(IDF_PATH)/components/esp_security/src/esp_hmac.c \
	$(IDF_PATH)/components/bt/common/tinycrypt/port/esp_tinycrypt_port.c \
	$(IDF_PATH)/components/esp_hal_security/hmac_hal.c \
	$(IDF_PATH)/components/esp_hal_security/ecdsa_hal.c
s31_sdk_ecc.c s31_sdk_mpi.c s31_sdk_ecdsa.c s31_sdk_hmac.c s31_sdk_tinycrypt.c s31_sdk_ecc.rsp s31_sdk_mpi.rsp s31_sdk_ecdsa.rsp s31_sdk_hmac.rsp s31_sdk_tinycrypt.rsp &: $(PROJECT_DIR)/tools/build/radio_crypto_adapters.py $(S31_SDK_CRYPTO_SOURCES) $(RADIO_BUILD_ABS)/compile_commands.json
	python3 $(PROJECT_DIR)/tools/build/radio_crypto_adapters.py --idf $(IDF_PATH) --build $(RADIO_BUILD_ABS) --output .
s31_sdk_%.o: s31_sdk_%.c s31_sdk_%.rsp $(RADIO_SRC)/s31_sdk_crypto.h $(PROJECT_DIR)/shared/s31_crypto.h
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -I $(RADIO_SRC) @s31_sdk_$*.rsp -c $< -o $@

s31_memcpy.o: $(RADIO_SRC)/s31_memcpy.c
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -c $< -o $@

# Check identity on every invocation, but preserve mtime when unchanged so a
# no-op build remains a no-op. IDF headers are tracked by compiler depfiles.
.PHONY: FORCE
FORCE:

.s31-build-config: FORCE
	python3 $(PROJECT_DIR)/tools/build/input_stamp.py --output $@ \
		--compiler "$(ESP_CC)" --repo "$(IDF_PATH)" \
		--file "$(RADIO_SDKCONFIG)" --file $(RADIO_SRC)/idf_includes.rsp \
		--file $(RADIO_SRC)/boot_flags.txt --file $(RADIO_SRC)/boot_linkpath.txt --file $(PROJECT_DIR)/configs/build-versions.mk \
		--value="$(ESP_CFLAGS) $(RADIO_STACK_DEFS)" \
		--value="$(HEAP_WRAP_FLAGS) $(LINUX_WRAP_FLAGS)" \
		--value="$(LINUX_NOFREERTOS_LINK) $(PICOLIBC_LIB) $(PICOLIBC_GCC)"

RADIO_LOCAL_OBJS := radio_stack.linux.o s31_vhci.o s31_vhci_pool_wake.o $(S31RTOS_LINUX_OBJS) $(RADIO_COEX_PRINTF_OBJ)
$(RADIO_LOCAL_OBJS): .s31-build-config $(RADIO_MAKEFILE)
$(filter s31_rtos/%,$(RADIO_LOCAL_OBJS)): | s31_rtos
s31_rtos:
	mkdir -p $@
linux_radio.raw.o: $(LINUX_NOFREERTOS_LINK) .s31-build-config \
	$(wildcard $(PICOLIBC_LIB)/libc.a $(PICOLIBC_LIB)/libm.a $(PICOLIBC_GCC)/libgcc.a)
-include $(RADIO_LOCAL_OBJS:.o=.d)

clean:
	rm -f *.o *.d s31_rtos/*.o s31_rtos/*.d .s31-build-config \
		linux-radio-undefined.txt linux-radio-localize.txt linux-radio-linked-imports.txt \
		nofreertos_link.txt undefined.txt $(LINUX_NET80211) \
		.s31-net80211/ieee80211_api.o

endif # CURDIR == RADIO_OUT
