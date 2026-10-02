# SPDX-License-Identifier: BSD-2-Clause
# ESP32-S31 Linux S-mode Wi-Fi/Bluetooth payload.
include ../../configs/build-versions.mk

IDF_DEPS_DIR ?= idf_deps
IDF_DEPS_ABS := $(abspath $(IDF_DEPS_DIR))
RADIO_BUILD  := $(IDF_DEPS_DIR)/build-radio
RADIO_BUILD_ABS := $(abspath $(RADIO_BUILD))
RADIO_SDKCONFIG := $(RADIO_BUILD)/config/sdkconfig.h
IDF_ROOT     ?= $(HOME)/.espressif
IDF_EXPORT   ?= $(IDF_ROOT)/master/esp-idf/export.sh
IDF_PATH     ?= $(patsubst %/export.sh,%,$(IDF_EXPORT))
# Match the compiler declared by the pinned IDF tools manifest.
ESP_TOOLS ?= $(IDF_ROOT)/tools/riscv32-esp-elf/$(ESP_ELF_RELEASE)/riscv32-esp-elf

# picolibc multilib matching the ilp32f radio world.
PICOLIBC_LIB := $(ESP_TOOLS)/picolibc/riscv32-esp-elf/lib/rv32imafc_zicsr_zifencei_zaamo_zalrsc/ilp32f
PICOLIBC_GCC := $(firstword $(wildcard $(ESP_TOOLS)/picolibc/lib/gcc/riscv32-esp-elf/*/rv32imafc_zicsr_zifencei_zaamo_zalrsc/ilp32f))

# The component closure is captured from IDF's generated build.  Its path
# placeholders are resolved at build time, so the capture has no developer-
# specific absolute paths.
IDF_CAPTURED_INCLUDES := $(shell sed -e 's|@IDF_PATH@|$(IDF_PATH)|g' -e 's|@RADIO_BUILD@|$(RADIO_BUILD_ABS)|g' idf_includes.rsp)
IDF_BOOT_LINKPATH := $(shell sed -e 's|@IDF_PATH@|$(IDF_PATH)|g' -e 's|@RADIO_BUILD@|$(RADIO_BUILD_ABS)|g' boot_linkpath.txt)
IDF_NET80211 := $(IDF_PATH)/components/esp_wifi/lib/esp32s31/libnet80211.a
LINUX_NET80211 := libnet80211.s31-linux.a
# Resolve one ordered archive set for both the linker and dependency graph.
# radio-idf-deps must finish before entering this component Makefile.
RADIO_ARCHIVES := $(shell python3 ../../tools/radio_archives.py boot_link.txt "$(IDF_PATH)" "$(RADIO_BUILD)")
LINUX_NOFREERTOS_LINK := $(subst $(IDF_NET80211),$(LINUX_NET80211),$(RADIO_ARCHIVES))

HEAP_WRAP_APIS := heap_caps_malloc heap_caps_free heap_caps_calloc \
	heap_caps_realloc heap_caps_aligned_alloc heap_caps_aligned_calloc \
	heap_caps_malloc_default heap_caps_realloc_default \
	heap_caps_malloc_prefer heap_caps_get_free_size \
	heap_caps_get_largest_free_block heap_caps_add_region
comma := ,
HEAP_WRAP_FLAGS := $(addprefix -Wl$(comma)--wrap=,$(HEAP_WRAP_APIS))
LINUX_WRAP_APIS := time esp_log_timestamp esp_timer_create esp_timer_delete \
	esp_timer_start_once esp_timer_start_once_at esp_timer_start_periodic \
	esp_timer_start_periodic_at esp_timer_stop esp_timer_restart \
	esp_timer_is_active esp_timer_get_time esp_intr_ptr_in_isr_region \
	esp_intr_alloc esp_intr_enable esp_intr_disable esp_intr_free \
	esp_ipc_call_blocking \
	mpi_hal_write_to_mem_block mpi_hal_write_at_offset mpi_hal_write_rinv \
	mpi_hal_read_result_hw_op \
	ieee80211_set_tx_desc lmacTxFrame hal_crypto_set_key_entry \
	lmacProcessTxError lmacTxDone esp_phy_enable esp_phy_disable \
	coex_enable coex_disable \
	coex_register_start_cb coex_register_ble_cb \
	coex_schm_process_restart coex_schm_register_callback \
	coex_hw_timer_set coex_hw_timer_enable coex_hw_timer_disable
LINUX_WRAP_FLAGS := $(addprefix -Wl$(comma)--wrap=,$(LINUX_WRAP_APIS))

.PHONY: all clean idf-check linux-radio linux-kbuild

all: linux-radio

idf-check:
	python3 ../../tools/check_build_versions.py --idf "$(IDF_PATH)" $(if $(filter 1,$(S31_ALLOW_UNPINNED)),--allow-unpinned)
	@test -f "$(IDF_PATH)/export.sh" || { echo "ERROR: ESP-IDF export.sh not found under $(IDF_ROOT)" >&2; exit 1; }
	@test -x "$(ESP_TOOLS)/bin/riscv32-esp-elf-gcc" || { echo "ERROR: ESP-IDF RISC-V toolchain with picolibc not found under $(IDF_ROOT)/tools" >&2; exit 1; }
	@test -d "$(PICOLIBC_LIB)" && test -d "$(PICOLIBC_GCC)" || { echo "ERROR: required ILP32F picolibc multilib is missing" >&2; exit 1; }
	@test -f "$(RADIO_BUILD)/build.ninja" || { echo "ERROR: missing radio-only IDF build at $(RADIO_BUILD)" >&2; exit 1; }

# Relocatable payload used by the Linux S-mode driver. It intentionally
# contains no OpenSBI objects and performs no M-mode setup.
# The final kernel integration prefixes its private IDF namespace and supplies
# the small set of remaining Linux OS-adapter entry points.
linux-radio: idf-check linux_radio.resolved.o linux_radio.kbuild.o linux-radio-undefined.txt

LINUX_RADIO_FW ?= ../../build/esp32s31-radio-fw-v1.o
LINUX_RADIO_IMPORTS ?= ../../linux-esp32-s31/drivers/platform/esp32s31-radio-imports.S
LINUX_RADIO_LINKED_IMPORTS ?= linux-radio-linked-imports.txt
RADIO_COEX_PRINTF_OBJ := s31_coex_printf.o

linux-kbuild: idf-check linux_radio.kbuild.o
	mkdir -p $(dir $(LINUX_RADIO_FW)) $(dir $(LINUX_RADIO_IMPORTS))
	@cmp -s linux_radio.kbuild.o $(LINUX_RADIO_FW) || cp -f linux_radio.kbuild.o $(LINUX_RADIO_FW)
	python3 ../../tools/gen_s31_radio_imports.py \
		--nm $(ESP_TOOLS)/bin/riscv32-esp-elf-nm \
		--linked-symbols $(LINUX_RADIO_LINKED_IMPORTS) \
		linux_radio.kbuild.o $(LINUX_RADIO_IMPORTS)

$(LINUX_NET80211): $(IDF_NET80211)
	cp $< $@

linux_radio.raw.o: radio_stack.linux.o nofreertos_link.txt boot_flags.txt boot_linkpath.txt $(LINUX_NET80211) $(S31RTOS_LINUX_OBJS) $(RADIO_COEX_PRINTF_OBJ) Makefile ../../tools/demote_riscv_module_tls.py prepare_xip_sections.py
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
		-Wl,--undefined=s31_linux_timer_next_due_us \
		-Wl,--undefined=s31_rtos_hard_tick \
		-Wl,--undefined=s31_rtos_free \
		-Wl,--undefined=s31_rtos_task_release \
		-Wl,--undefined=s31_rtos_isr_depth \
		-Wl,--undefined=xTaskCreatePinnedToCore \
		$(shell cat boot_flags.txt) \
		$(IDF_BOOT_LINKPATH) \
		radio_stack.linux.o $(S31RTOS_LINUX_OBJS) $(RADIO_COEX_PRINTF_OBJ) \
		$(LINUX_NOFREERTOS_LINK) \
		-L$(PICOLIBC_LIB) -L$(PICOLIBC_GCC) -lc -lm -lgcc -o $@
	python3 ../../tools/demote_riscv_module_tls.py $@
	python3 prepare_xip_sections.py $@

linux_radio.exposed.o: linux_radio.raw.o
	$(ESP_TOOLS)/bin/riscv32-esp-elf-objcopy \
		--globalize-symbol=wifi_on_coex_start_process \
		--globalize-symbol=wifi_on_coex_schm_phase_process $< $@

# IDF normally reaches the Wi-Fi workers through esp_wifi_ipc_internal().  S31
# Linux serializes radio work on one worker, so expose those two execution-
# context entry points.  The closed coexist scheme's existing esp_timer adapter
# is dispatched by the compatibility timer bridge; it must not be advanced a
# second time here.
linux_radio.resolved.o: linux_radio.exposed.o
	$(ESP_TOOLS)/bin/riscv32-esp-elf-ld -r --no-relax $< -o $@

linux-radio-undefined.txt: linux_radio.resolved.o
	$(ESP_TOOLS)/bin/riscv32-esp-elf-nm -u $< | sort -u > $@

linux-radio-localize.txt: linux_radio.resolved.o Makefile
	$(ESP_TOOLS)/bin/riscv32-esp-elf-nm -g --defined-only $< | \
		awk '{ n=$$NF; if (n !~ /^(s31_radio_fw_abi_version|s31_radio_(stack|shutdown|bt_(enable|disable)|wifi_(scan|connect|disconnect|control))_task|s31_radio_wifi_(read_mac|clock_enable|clock_disable)|s31_radio_(vhci|wifi)_try_send|s31_radio_wifi_try_send_interface|s31_radio_coex_status|s31_rtos_(init|tick|hard_tick|free|task_release|isr_depth)|s31_linux_timer_next_due_us|xTaskCreatePinnedToCore)$$/) print n; }' | \
		sort -u > $@

linux_radio.localized.o: linux_radio.resolved.o linux-radio-localize.txt
	$(ESP_TOOLS)/bin/riscv32-esp-elf-objcopy \
		--strip-debug \
		--localize-symbols=linux-radio-localize.txt $< $@

linux_radio.kbuild.o: linux_radio.localized.o linux_module_sections.lds
	$(ESP_TOOLS)/bin/riscv32-esp-elf-ld -r --no-relax \
		-T linux_module_sections.lds $< -o $@

S31RTOS_LINUX_OBJS := s31_rtos/s31_rtos_core.linux.o \
		s31_rtos/s31_rtos_queue.o s31_rtos/s31_rtos_event.o \
		s31_linux_locks.o s31_linux_timer.o s31_memcpy.o
# The main rule appears above this simply-expanded list; add the prerequisites
# here so a clean/forced build cannot reach the linker before they exist.
linux_radio.raw.o: $(S31RTOS_LINUX_OBJS)
s31_coex_printf.o: $(IDF_PATH)/components/esp_coex/src/lib_printf.c
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -c $< -o $@
s31_rtos/%.o: s31_rtos/%.c s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -I s31_rtos -c $< -o $@

s31_rtos/s31_rtos_core.linux.o: s31_rtos/s31_rtos_core.c s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -DS31_LINUX_SMODE -I s31_rtos -c $< -o $@

# Keep a readable record of precisely the archives linked into the payload.
nofreertos_link.txt: boot_link.txt $(RADIO_BUILD)/build.ninja .s31-build-config
	@printf '%s\n' $(LINUX_NOFREERTOS_LINK) > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@

ESP_CC      := $(ESP_TOOLS)/bin/riscv32-esp-elf-gcc
ifeq ($(S31_WIFI_ONLY),1)
RADIO_STACK_DEFS := -DS31_LINUX_SMODE -DS31_WIFI_ONLY
else
RADIO_STACK_DEFS := -DS31_LINUX_SMODE
endif
ESP_CFLAGS  := -Os -mabi=ilp32f -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p2 \
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
radio_stack.linux.o: radio_stack.c s31_softmac.inc s31_softmac_tx_agg.inc s31_softmac_tx_reports.inc ../../linux-esp32-s31/include/linux/esp32s31-radio-control.h s31_rtos/s31_rtos.h Makefile $(RADIO_SDKCONFIG)
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP $(RADIO_STACK_DEFS) \
		-I s31_rtos -c $< -o $@

s31_linux_locks.o: s31_linux_locks.c s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -DS31_LINUX_SMODE -I s31_rtos -c $< -o $@

s31_linux_timer.o: s31_linux_timer.c s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -DS31_LINUX_SMODE -I s31_rtos -c $< -o $@

s31_memcpy.o: s31_memcpy.c
	$(ESP_CC) $(ESP_CFLAGS) -MMD -MP -c $< -o $@

# Check identity on every invocation, but preserve mtime when unchanged so a
# no-op build remains a no-op. IDF headers are tracked by compiler depfiles.
.PHONY: FORCE
FORCE:

.s31-build-config: FORCE
	python3 ../../tools/write_build_stamp.py --output $@ \
		--compiler "$(ESP_CC)" --repo "$(IDF_PATH)" \
		--file "$(RADIO_SDKCONFIG)" --file idf_includes.rsp \
		--file boot_flags.txt --file boot_linkpath.txt --file ../../configs/build-versions.mk \
		--value="$(ESP_CFLAGS) $(RADIO_STACK_DEFS)" \
		--value="$(HEAP_WRAP_FLAGS) $(LINUX_WRAP_FLAGS)" \
		--value="$(LINUX_NOFREERTOS_LINK) $(PICOLIBC_LIB) $(PICOLIBC_GCC)"

RADIO_LOCAL_OBJS := radio_stack.linux.o $(S31RTOS_LINUX_OBJS) $(RADIO_COEX_PRINTF_OBJ)
$(RADIO_LOCAL_OBJS): .s31-build-config Makefile
linux_radio.raw.o: $(LINUX_NOFREERTOS_LINK) .s31-build-config \
	$(wildcard $(PICOLIBC_LIB)/libc.a $(PICOLIBC_LIB)/libm.a $(PICOLIBC_GCC)/libgcc.a)
-include $(RADIO_LOCAL_OBJS:.o=.d)

clean:
	rm -f *.o *.d s31_rtos/*.o s31_rtos/*.d .s31-build-config \
		linux-radio-undefined.txt linux-radio-localize.txt \
		nofreertos_link.txt undefined.txt $(LINUX_NET80211) \
		.s31-net80211/ieee80211_api.o
