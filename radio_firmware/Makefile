# SPDX-License-Identifier: BSD-2-Clause
# ESP32-S31 Linux S-mode Wi-Fi/Bluetooth payload.

BOOTLOADER   ?= ../bootloader
BOOTLOADER_ABS := $(abspath $(BOOTLOADER))
RADIO_BUILD  := $(BOOTLOADER)/build-radio
RADIO_BUILD_ABS := $(abspath $(RADIO_BUILD))
IDF_ROOT     ?= $(HOME)/.espressif
IDF_EXPORT   ?= $(firstword $(wildcard $(IDF_ROOT)/master/esp-idf/export.sh) $(shell find $(IDF_ROOT) -maxdepth 5 -type f -path '*/esp-idf/export.sh' 2>/dev/null | sort | head -n 1))
IDF_PATH     ?= $(patsubst %/export.sh,%,$(IDF_EXPORT))
# Prefer a complete ESP-IDF toolchain: the newest installed compiler need not
# carry the picolibc multilib used by this ILP32F payload.
ESP_TOOLS    ?= $(shell find "$(IDF_ROOT)/tools/riscv32-esp-elf" -type f -path '*/bin/riscv32-esp-elf-gcc' 2>/dev/null | sort | while IFS= read -r cc; do d=$$(dirname "$$(dirname "$$cc")"); if test -d "$$d/picolibc/riscv32-esp-elf/lib/rv32imafc_zicsr_zifencei_zaamo_zalrsc/ilp32f"; then printf '%s' "$$d"; break; fi; done)

# picolibc multilib matching the ilp32f radio world.
PICOLIBC_LIB := $(ESP_TOOLS)/picolibc/riscv32-esp-elf/lib/rv32imafc_zicsr_zifencei_zaamo_zalrsc/ilp32f
PICOLIBC_GCC := $(firstword $(wildcard $(ESP_TOOLS)/picolibc/lib/gcc/riscv32-esp-elf/*/rv32imafc_zicsr_zifencei_zaamo_zalrsc/ilp32f))

# The component closure is captured from IDF's generated build.  Its path
# placeholders are resolved at build time, so the capture has no developer-
# specific absolute paths.
IDF_CAPTURED_INCLUDES := $(shell sed -e 's|@IDF_PATH@|$(IDF_PATH)|g' -e 's|@RADIO_BUILD@|$(RADIO_BUILD_ABS)|g' idf_includes.rsp)
IDF_BOOT_LINKPATH := $(shell sed -e 's|@IDF_PATH@|$(IDF_PATH)|g' -e 's|@RADIO_BUILD@|$(RADIO_BUILD_ABS)|g' boot_linkpath.txt)

HEAP_WRAP_APIS := heap_caps_malloc heap_caps_free heap_caps_calloc \
	heap_caps_realloc heap_caps_aligned_alloc heap_caps_aligned_calloc \
	heap_caps_malloc_default heap_caps_realloc_default \
	heap_caps_malloc_prefer heap_caps_get_free_size \
	heap_caps_get_largest_free_block heap_caps_add_region
comma := ,
HEAP_WRAP_FLAGS := $(addprefix -Wl$(comma)--wrap=,$(HEAP_WRAP_APIS))
LINUX_WRAP_APIS := esp_log_timestamp esp_timer_create esp_timer_delete \
	esp_timer_start_once esp_timer_start_once_at esp_timer_start_periodic \
	esp_timer_start_periodic_at esp_timer_stop esp_timer_restart \
	esp_timer_is_active esp_timer_get_time esp_intr_ptr_in_isr_region \
	esp_intr_alloc esp_intr_enable esp_intr_disable esp_intr_free \
	ieee80211_set_tx_desc lmacTxFrame hal_crypto_set_key_entry \
	lmacProcessTxError lmacTxDone
LINUX_WRAP_FLAGS := $(addprefix -Wl$(comma)--wrap=,$(LINUX_WRAP_APIS))

.PHONY: all clean idf-check linux-radio linux-kbuild

all: linux-radio

idf-check:
	@test -f "$(IDF_PATH)/export.sh" || { echo "ERROR: ESP-IDF export.sh not found under $(IDF_ROOT)" >&2; exit 1; }
	@test -x "$(ESP_TOOLS)/bin/riscv32-esp-elf-gcc" || { echo "ERROR: ESP-IDF RISC-V toolchain with picolibc not found under $(IDF_ROOT)/tools" >&2; exit 1; }
	@test -d "$(PICOLIBC_LIB)" && test -d "$(PICOLIBC_GCC)" || { echo "ERROR: required ILP32F picolibc multilib is missing" >&2; exit 1; }
	@test -f "$(RADIO_BUILD)/build.ninja" || { echo "ERROR: missing radio-only IDF build at $(RADIO_BUILD)" >&2; exit 1; }

# Relocatable payload used by the Linux S-mode driver. It intentionally
# contains no OpenSBI objects and performs no M-mode setup.
# The final kernel integration prefixes its private IDF namespace and supplies
# the small set of remaining Linux OS-adapter entry points.
linux-radio: idf-check linux_radio.raw.o linux_radio.kbuild.o linux-radio-undefined.txt

LINUX_IDF_OBJ ?= ../linux-esp32-s31/drivers/platform/esp32s31-radio-idf.o_shipped
RADIO_COEX_PRINTF_OBJ := s31_coex_printf.o

linux-kbuild: idf-check linux_radio.kbuild.o
	cp -f linux_radio.kbuild.o $(LINUX_IDF_OBJ)

linux_radio.raw.o: radio_stack.linux.o nofreertos_link.txt boot_flags.txt boot_linkpath.txt $(S31RTOS_LINUX_OBJS) $(RADIO_COEX_PRINTF_OBJ) Makefile
	$(ESP_CC) -nostdlib -r -Wl,--gc-sections $(HEAP_WRAP_FLAGS) $(LINUX_WRAP_FLAGS) \
		-Wl,--undefined=s31_radio_stack_task \
		-Wl,--undefined=s31_radio_wifi_scan_task \
		-Wl,--undefined=s31_radio_wifi_connect_task \
		-Wl,--undefined=s31_radio_wifi_disconnect_task \
		-Wl,--undefined=s31_radio_wifi_read_mac \
		-Wl,--undefined=s31_radio_wifi_clock_enable \
		-Wl,--undefined=s31_radio_wifi_clock_disable \
		-Wl,--undefined=s31_radio_wifi_try_send \
		-Wl,--undefined=s31_radio_wifi_free_rx_buffer \
		-Wl,--undefined=s31_radio_bt_enable_task \
		-Wl,--undefined=s31_radio_vhci_try_send \
		-Wl,--undefined=s31_rtos_init \
		-Wl,--undefined=s31_rtos_tick \
		-Wl,--undefined=s31_rtos_hard_tick \
		-Wl,--undefined=s31_rtos_task_release \
		-Wl,--undefined=xPortEnterCriticalTimeout \
		-Wl,--undefined=vPortExitCriticalMultiCore \
		-Wl,--undefined=wifi_module_enable \
		-Wl,--undefined=wifi_module_disable \
		-Wl,--undefined=esp_wifi_init \
		-Wl,--undefined=esp_intr_alloc \
		$(shell cat boot_flags.txt) \
		$(IDF_BOOT_LINKPATH) \
		radio_stack.linux.o $(S31RTOS_LINUX_OBJS) $(RADIO_COEX_PRINTF_OBJ) \
		$(shell sed 's|^esp-idf/|$(RADIO_BUILD)/esp-idf/|' nofreertos_link.txt) \
		-L$(PICOLIBC_LIB) -L$(PICOLIBC_GCC) -lc -lm -lgcc -o $@

linux-radio-undefined.txt: linux_radio.raw.o
	$(ESP_TOOLS)/bin/riscv32-esp-elf-nm -u $< | sort -u > $@

linux-radio-localize.txt: linux_radio.raw.o
	$(ESP_TOOLS)/bin/riscv32-esp-elf-nm -g --defined-only $< | \
		awk '{ n=$$NF; if (n !~ /^(s31_radio_(stack|bt_enable|wifi_(scan|connect|disconnect))_task|s31_radio_wifi_(read_mac|clock_enable|clock_disable|free_rx_buffer)|s31_radio_(vhci|wifi)_try_send|s31_rtos_(init|tick|hard_tick|free|task_release|isr_depth)|xTaskCreatePinnedToCore)$$/) print n; }' | \
		sort -u > $@

linux_radio.kbuild.o: linux_radio.raw.o linux-radio-localize.txt
	$(ESP_TOOLS)/bin/riscv32-esp-elf-objcopy \
		--strip-debug \
		--localize-symbols=linux-radio-localize.txt $< $@

S31RTOS_LINUX_OBJS := s31_rtos/s31_rtos_core.linux.o \
		s31_rtos/s31_rtos_queue.o s31_rtos/s31_rtos_event.o \
		s31_linux_locks.o s31_linux_timer.o s31_memcpy.o
# The main rule appears above this simply-expanded list; add the prerequisites
# here so a clean/forced build cannot reach the linker before they exist.
linux_radio.raw.o: $(S31RTOS_LINUX_OBJS)
s31_coex_printf.o: $(IDF_PATH)/components/esp_coex/src/lib_printf.c
	$(ESP_CC) $(ESP_CFLAGS) -c $< -o $@
s31_rtos/%.o: s31_rtos/%.c s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -I s31_rtos -c $< -o $@

s31_rtos/s31_rtos_core.linux.o: s31_rtos/s31_rtos_core.c s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -DS31_LINUX_SMODE -I s31_rtos -c $< -o $@

# Preserve the minimal, previously working archive order, but only use
# components produced by the matching radio-only IDF configuration.  Pulling
# the full application closure into a relocatable Linux payload also drags in
# IDF startup/restart code and linker-script boundary symbols that do not
# belong in S mode.
nofreertos_link.txt: boot_link.txt $(RADIO_BUILD)/build.ninja
	@set -eu; : > $@; \
	sed 's|@IDF_PATH@|$(IDF_PATH)|g' $< | while IFS= read -r archive; do \
		case "$$archive" in \
			esp-idf/freertos/libfreertos.a) continue ;; \
			esp-idf/*.a) candidate="$(RADIO_BUILD)/$$archive" ;; \
			/*.a) candidate="$$archive" ;; \
			*) continue ;; \
		esac; \
	if [ -f "$$candidate" ]; then printf '%s\n' "$$archive" >> $@; fi; \
	done

ESP_CC      := $(ESP_TOOLS)/bin/riscv32-esp-elf-gcc
ifeq ($(S31_WIFI_ONLY),1)
RADIO_STACK_DEFS := -DS31_LINUX_SMODE -DS31_WIFI_ONLY
else
RADIO_STACK_DEFS := -DS31_LINUX_SMODE
endif
ESP_CFLAGS  := -mabi=ilp32f -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p2 \
		-mtune=esp-base -mespv-spec=2p2 -ffunction-sections -fdata-sections \
		-fno-stack-protector -fno-builtin -ffreestanding -std=gnu11 -Wall \
	-specs=$(RADIO_BUILD)/specs/picolibc.specs \
		-I$(IDF_PATH)/components/bt/include/esp32s31/include \
		-I$(IDF_PATH)/components/bt/porting_btdm/controller/btdm_common/include \
		-I$(IDF_PATH)/components/bt/porting_btdm/controller/ble/include \
		-I$(IDF_PATH)/components/bt/porting_btdm/controller/bredr/include \
		-I$(IDF_PATH)/components/bt/porting_btdm/transport/include \
		-I$(IDF_PATH)/components/esp_hal_timg/include \
		-I$(IDF_PATH)/components/esp_hal_timg/esp32s31/include \
		$(IDF_CAPTURED_INCLUDES)
radio_stack.linux.o: radio_stack.c s31_rtos/s31_rtos.h Makefile
	$(ESP_CC) $(ESP_CFLAGS) $(RADIO_STACK_DEFS) \
		-I s31_rtos -c $< -o $@

s31_linux_locks.o: s31_linux_locks.c s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -DS31_LINUX_SMODE -I s31_rtos -c $< -o $@

s31_linux_timer.o: s31_linux_timer.c s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -DS31_LINUX_SMODE -I s31_rtos -c $< -o $@

s31_memcpy.o: s31_memcpy.c
	$(ESP_CC) $(ESP_CFLAGS) -c $< -o $@

clean:
	rm -f *.o s31_rtos/*.o \
		linux-radio-undefined.txt linux-radio-localize.txt \
		nofreertos_link.txt undefined.txt
