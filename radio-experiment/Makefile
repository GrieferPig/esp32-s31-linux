# SPDX-License-Identifier: BSD-2-Clause
# ESP32-S31 Linux S-mode Wi-Fi/Bluetooth payload.

BOOTLOADER   ?= ../bootloader
IDF_PATH     ?= $(HOME)/.espressif/master/esp-idf
ESP_TOOLS    ?= $(HOME)/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf
BT_LIBS      := $(BOOTLOADER)/build/esp-idf/bt/libbt.a \
	$(shell find $(IDF_PATH)/components/bt/controller/lib_esp32s31/esp32s31-bt-lib -name 'lib*.a' | sort)

# picolibc multilib matching the ilp32f radio world.
PICOLIBC_LIB := $(ESP_TOOLS)/picolibc/riscv32-esp-elf/lib/rv32imafc_zicsr_zifencei_zaamo_zalrsc/ilp32f
PICOLIBC_GCC := $(ESP_TOOLS)/picolibc/lib/gcc/riscv32-esp-elf/15.2.0/rv32imafc_zicsr_zifencei_zaamo_zalrsc/ilp32f

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
	esp_intr_alloc esp_intr_enable esp_intr_disable esp_intr_free
LINUX_WRAP_FLAGS := $(addprefix -Wl$(comma)--wrap=,$(LINUX_WRAP_APIS))

.PHONY: all clean linux-radio linux-kbuild

all: linux-radio

# Relocatable payload used by the Linux S-mode driver. It intentionally
# contains no OpenSBI objects and performs no M-mode setup.
# The final kernel integration prefixes its private IDF namespace and supplies
# the small set of remaining Linux OS-adapter entry points.
linux-radio: linux_radio.raw.o linux_radio.kbuild.o linux-radio-undefined.txt

LINUX_IDF_OBJ ?= ../linux-esp32-s31/drivers/platform/esp32s31-radio-idf.o_shipped

linux-kbuild: linux_radio.kbuild.o
	cp -f linux_radio.kbuild.o $(LINUX_IDF_OBJ)

linux_radio.raw.o: radio_stack.linux.o nofreertos_link.txt boot_flags.txt boot_linkpath.txt $(S31RTOS_LINUX_OBJS)
	$(ESP_CC) -nostdlib -r -Wl,--gc-sections $(HEAP_WRAP_FLAGS) $(LINUX_WRAP_FLAGS) \
		-Wl,--undefined=s31_radio_stack_task \
		-Wl,--undefined=s31_radio_bt_enable_task \
		-Wl,--undefined=s31_radio_vhci_try_send \
		-Wl,--undefined=s31_radio_wifi_scan_task \
		-Wl,--undefined=s31_rtos_init -Wl,--undefined=s31_rtos_schedule \
		-Wl,--undefined=s31_rtos_tick \
		-Wl,--undefined=esp_wifi_init -Wl,--undefined=esp_bt_controller_init \
		-Wl,--undefined=esp_intr_alloc \
		$(shell cat boot_flags.txt) $(shell cat boot_linkpath.txt) \
		radio_stack.linux.o $(S31RTOS_LINUX_OBJS) $(BT_LIBS) \
		$(shell sed 's|^esp-idf/|$(BOOTLOADER)/build/esp-idf/|' nofreertos_link.txt) \
		-L$(PICOLIBC_LIB) -L$(PICOLIBC_GCC) -lc -lm -lgcc -o $@

linux-radio-undefined.txt: linux_radio.raw.o
	$(ESP_TOOLS)/bin/riscv32-esp-elf-nm -u $< | sort -u > $@

linux-radio-localize.txt: linux_radio.raw.o
	$(ESP_TOOLS)/bin/riscv32-esp-elf-nm -g --defined-only $< | \
		awk '{ n=$$NF; if (n !~ /^(s31_radio_(stack|bt_enable|wifi_scan)_task|s31_radio_vhci_try_send|s31_rtos_(init|schedule|tick|isr_depth)|xTaskCreatePinnedToCore)$$/) print n; }' | \
		sort -u > $@

linux_radio.kbuild.o: linux_radio.raw.o linux-radio-localize.txt
	$(ESP_TOOLS)/bin/riscv32-esp-elf-objcopy \
		--strip-debug \
		--localize-symbols=linux-radio-localize.txt $< $@

S31RTOS_LINUX_OBJS := s31_rtos/s31_rtos_core.linux.o \
		s31_rtos/s31_rtos_queue.o s31_rtos/s31_rtos_event.o \
		s31_linux_locks.o s31_linux_timer.o
# The main rule appears above this simply-expanded list; add the prerequisites
# here so a clean/forced build cannot reach the linker before they exist.
linux_radio.raw.o: $(S31RTOS_LINUX_OBJS)
s31_rtos/%.o: s31_rtos/%.c s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -I s31_rtos -c $< -o $@

s31_rtos/s31_rtos_core.linux.o: s31_rtos/s31_rtos_core.c s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -DS31_LINUX_SMODE -I s31_rtos -c $< -o $@

# Link variant without FreeRTOS: drop libfreertos.a from the archive set.
nofreertos_link.txt: boot_link.txt
	grep -E '^(esp-idf/|/).*[.]a$$' $< | \
		grep -v '^esp-idf/freertos/libfreertos.a$$' > $@

ESP_CC      := $(ESP_TOOLS)/bin/riscv32-esp-elf-gcc
ESP_CFLAGS  := -mabi=ilp32f -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p2 \
		-mtune=esp-base -mespv-spec=2p2 -ffunction-sections -fdata-sections \
		-fno-stack-protector -fno-builtin -ffreestanding -std=gnu11 -Wall \
		-specs=$(BOOTLOADER)/build/specs/picolibc.specs \
		-I$(IDF_PATH)/components/bt/include/esp32s31/include \
		-I$(IDF_PATH)/components/bt/porting_btdm/controller/btdm_common/include \
		-I$(IDF_PATH)/components/bt/porting_btdm/controller/ble/include \
		-I$(IDF_PATH)/components/esp_hal_timg/include \
		-I$(IDF_PATH)/components/esp_hal_timg/esp32s31/include \
		$(shell cat idf_includes.rsp)
radio_stack.linux.o: radio_stack.c s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -DS31_LINUX_SMODE -I s31_rtos -c $< -o $@

s31_linux_locks.o: s31_linux_locks.c s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -DS31_LINUX_SMODE -I s31_rtos -c $< -o $@

s31_linux_timer.o: s31_linux_timer.c s31_rtos/s31_rtos.h
	$(ESP_CC) $(ESP_CFLAGS) -DS31_LINUX_SMODE -I s31_rtos -c $< -o $@

clean:
	rm -f *.o s31_rtos/*.o \
		linux-radio-undefined.txt linux-radio-localize.txt \
		nofreertos_link.txt undefined.txt
