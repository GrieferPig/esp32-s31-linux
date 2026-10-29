/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Coalesce ESP-IDF's per-function/data sections before Kbuild.  Keeping the
 * thousands of input sections in the final ET_REL module makes relocation
 * metadata several times larger than the code and exhausts the S31's 16 MiB
 * RAM during load.  The payload has already been garbage-collected by the
 * preceding partial link, so this changes layout only, not reachability.
 */
SECTIONS
{
	.text : {
		*(.text .text.*)
		*(.iram1 .iram1.*)
		*(.wifi0iram .wifi0iram.*)
		*(.wifirxiram .wifirxiram.*)
		*(.wifiextrairam .wifiextrairam.*)
		*(.wifislpiram .wifislpiram.*)
		*(.wifislprxiram .wifislprxiram.*)
		*(.coexiram .coexiram.*)
	}

	.rodata : {
		*(.rodata .rodata.*)
		*(.srodata .srodata.*)
		*(.rodata_wlog_debug .rodata_wlog_debug.*)
		*(.rodata_wlog_info .rodata_wlog_info.*)
		*(.rodata_wlog_error .rodata_wlog_error.*)
		*(.rodata_wlog_warning .rodata_wlog_warning.*)
		*(.rodata_wlog_verbose .rodata_wlog_verbose.*)
	}

	.data : {
		*(.data .data.*)
		*(.sdata .sdata.*)
		*(.dram1 .dram1.*)
		*(.tdata .tdata.*)
		*(.rtc_timer_data_in_rtc_mem)
	}

	.bss (NOLOAD) : {
		*(.bss .bss.*)
		*(.sbss .sbss.*)
		*(COMMON)
	}

	.init_array : { KEEP(*(.init_array .init_array.*)) }
}
