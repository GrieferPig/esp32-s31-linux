#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Prelink the complete radio closure into an independently flashable XIP slot.

Code/constant relocations are resolved on the host. Module callbacks go through
fixed SRAM trampolines backed by writable import slots in the built-in arena.
The kernel's arena address is part of the image contract, not a runtime guess.
"""
from __future__ import annotations

import argparse
import json
from io import BytesIO
from elftools.elf.elffile import ELFFile
from pathlib import Path
import re
import struct
import subprocess
import zlib

MAGIC = 0x58313353  # S31X
BASE = 0xBE06E000
HEADER_SIZE = 4096
SLOT_SIZE = 0x1C0000
RAM_SIZE = 40960
# Leave a gap after the 4 MiB flash vmalloc reservation and its guard page.
WIFI_IRAM_BASE = 0xBE420000
WIFI_IRAM_CAPACITY = 0x11800
IRAM_FIELDS_OFFSET = 4032
EXPORTS = (
    's31_radio_stack_task', 's31_radio_bt_enable_task',
    's31_radio_bt_disable_task', 's31_radio_shutdown_task',
    's31_radio_vhci_try_send', 's31_radio_coex_status',
    's31_radio_wifi_scan_task', 's31_radio_wifi_connect_task',
    's31_radio_wifi_disconnect_task', 's31_radio_wifi_control_task',
    's31_radio_wifi_try_send_interface', 's31_radio_wifi_read_mac',
    's31_radio_wifi_clock_enable', 's31_radio_wifi_clock_disable',
    's31_radio_wifi_try_send', 's31_rtos_init', 's31_rtos_tick',
    's31_linux_timer_next_deadline_ns', 's31_rtos_hard_tick', 's31_rtos_free',
    's31_rtos_task_release', 'xTaskCreatePinnedToCore', 's31_rtos_isr_depth',
)
EMPTY_CLOCK_IMPORTS = frozenset(('_esp_libc_clock_array_start', '_esp_libc_clock_array_end'))

def read_elf(path):
    elf = ELFFile(BytesIO(path.read_bytes()))
    if elf.elfclass != 32 or not elf.little_endian or elf['e_machine'] != 'EM_RISCV':
        raise ValueError('expected little-endian RISC-V ELF32')
    return elf


def read_symbols(elf):
    return {symbol.name: symbol for symbol in elf.get_section_by_name('.symtab').iter_symbols()
            if symbol.name}

def select_iram_sections(payload, requested):
    """Select whole executable input sections; report co-resident functions too."""
    sections = list(payload.iter_sections())
    names = [section.name for section in sections]
    symbols = read_symbols(payload)
    selected = set()
    for name in requested:
        if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9.]*', name):
            raise ValueError('invalid SRAM function name: ' + name)
        symbol = symbols.get(name)
        index = symbol['st_shndx'] if symbol is not None else None
        if (symbol is None or symbol['st_info']['type'] != 'STT_FUNC' or
                not isinstance(index, int) or not 0 < index < len(sections)):
            raise ValueError('SRAM target is not a defined payload function: ' + name)
        section = sections[index]
        if (section['sh_type'] != 'SHT_PROGBITS' or section['sh_flags'] & 7 != 6 or
                not section['sh_size'] or not re.fullmatch(r'\.[A-Za-z_0-9.]+', section.name)):
            raise ValueError('SRAM target section is not read-only executable code: ' + name)
        selected.add(index)
    if not selected:
        raise ValueError('empty SRAM function selection')
    # objcopy renames by name, so ambiguous section names would move extra code.
    if any(names.count(names[index]) != 1 for index in selected):
        raise ValueError('ambiguous SRAM input section name')
    return [dict(section=sections[index].name, bytes=sections[index]['sh_size'],
                 functions=sorted(name for name, symbol in symbols.items()
                                  if symbol['st_shndx'] == index and
                                  symbol['st_info']['type'] == 'STT_FUNC'))
            for index in sorted(selected)]

def run(*args):
    subprocess.run([str(a) for a in args], check=True)

def build(args):
    out = args.output.parent
    out.mkdir(parents=True, exist_ok=True)
    prefix = args.prefix
    kernel = read_elf(args.kernel)
    kernel_symbols = read_symbols(kernel)
    ram = kernel_symbols['esp32s31_radio_xip_ram']['st_value']
    if kernel_symbols['esp32s31_radio_xip_ram']['st_size'] != RAM_SIZE:
        raise ValueError('kernel XIP arena size differs')
    linked = {line.strip() for line in args.imports.read_text().splitlines()
              if line.strip() and not line.startswith('#')}
    # Fixed production placement; missing targets fail rather than fall back to flash.
    manifest = args.sram_symbols
    payload = read_elf(args.payload)
    requested = sorted(set(line.split('#', 1)[0].strip()
                           for line in manifest.read_text().splitlines()) - {''})
    selection = select_iram_sections(payload, requested)
    keep_iram_symbols = [n for rec in selection for n in rec['functions']]
    # Drop unused undefined symbols before generating the binding contract.
    stripped = out / 'radio-xip-input.o'
    run(prefix + 'objcopy', '--strip-unneeded', '--remove-section=.comment',
        *['--keep-symbol=' + name
          for name in (*EXPORTS, 's31_radio_fw_abi_version', *keep_iram_symbols)],
        args.payload, stripped)
    if selection:
        renamed = out / 'radio-xip-selected.o'
        run(prefix + 'objcopy',
            *['--rename-section=' + rec['section'] + '=.s31_tx_iram.' + str(i)
              for i, rec in enumerate(selection)], stripped, renamed)
        stripped = renamed
    payload = read_elf(stripped)
    payload_symbols = read_symbols(payload)
    abi = payload_symbols['s31_radio_fw_abi_version']
    abi_section = payload.get_section(abi['st_shndx'])
    if abi['st_size'] != 4 or struct.unpack_from('<I', abi_section.data(), abi['st_value'])[0] != 2:
        raise ValueError('unsupported radio payload ABI')
    for name in EXPORTS[:-1]:
        if payload_symbols[name]['st_info']['type'] != 'STT_FUNC':
            raise ValueError('radio export is not a function: ' + name)
    undefined = sorted(name for name, symbol in payload_symbols.items() if symbol['st_shndx'] == 'SHN_UNDEF')
    functions = [n for n in undefined if n in linked and
                 n not in ('_mtvt_table', '_esp_err_msg_tbl_start', '_esp_err_msg_tbl_end')]
    nullable = [n for n in undefined if n not in linked]
    for name in nullable:
        if name not in EMPTY_CLOCK_IMPORTS and payload_symbols[name]['st_info']['bind'] != 'STB_WEAK':
            raise ValueError('required import missing from binding contract: ' + name)
    for name in functions:
        if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name):
            raise ValueError('invalid import name')
    imports_section = '.s31_tx_iram.imports'
    asm = ['.option norelax', f'.section {imports_section},"ax",@progbits', '.balign 4']
    for i, name in enumerate(functions):
        asm += [f'.globl {name}', f'.type {name},@function', f'{name}:',
                f'lui t0,%hi(__xip_import_slots+{i * 4})',
                f'lw t0,%lo(__xip_import_slots+{i * 4})(t0)',
                'j __s31_import_tp_stack2' if name == 's31_linux_task_create_finalized'
                else 'j __s31_import_tp_guard']
    # All current imports use register arguments except finalized task
    # creation (ten XLEN words). Logging passes a va_list pointer instead of
    # an unbounded variadic tail. malloc_prefer ignores its preference tail
    # in the current all-SRAM allocator. Preserve real ABI stack arguments
    # only on a clobbered-TP slow path; never read beyond the caller's args.
    asm += ['.type __s31_import_tp_guard,@function', '__s31_import_tp_guard:',
            'csrr t1,sscratch', 'beq tp,t1,3f', 'addi sp,sp,-16', 'j 2f',
            '.type __s31_import_tp_stack2,@function', '__s31_import_tp_stack2:',
            'csrr t1,sscratch', 'beq tp,t1,3f', 'addi sp,sp,-16',
            'lw t2,16(sp)', 'sw t2,0(sp)', 'lw t2,20(sp)', 'sw t2,4(sp)',
            '2:', 'sw ra,12(sp)', 'sw tp,8(sp)', 'mv tp,t1',
            'jalr t0', 'lw tp,8(sp)', 'lw ra,12(sp)', 'addi sp,sp,16', 'ret',
            '3:', 'jr t0']
    asm += ['.section .xip_import_slots,"aw",@progbits', '.balign 4',
            '.globl __xip_import_slots', '__xip_import_slots:', f'.zero {len(functions) * 4}',
            '.section .xip_vectors,"aw",@progbits', '.balign 4',
            '.globl _mtvt_table', '_mtvt_table:', '.zero 192']
    (out / 'radio-xip-imports.S').write_text('\n'.join(asm) + '\n')
    run(prefix + 'gcc', '-c', '-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f',
        out / 'radio-xip-imports.S', '-o', out / 'radio-xip-imports.o')
    # RAM VMA and flash LMA are deliberately independent. The pristine writable
    # template remains in flash and is reused on resume, without a RAM snapshot.
    text_inputs = '*(.text .text.* .iram1 .iram1.* .wifi*iram* .coexiram*)'
    iram_output = f'''
      . = ALIGN(16); __xip_iram_load = .;
      .s31_wifi_iram {WIFI_IRAM_BASE} : AT(__xip_iram_load) {{
        __xip_iram_start = .;
        *(.s31_tx_iram.*)
        . = ALIGN(16); __xip_iram_end = .;
      }}
      __xip_data_load = ALIGN(__xip_iram_load + SIZEOF(.s31_wifi_iram), 16);'''
    script = f'''SECTIONS {{
      . = {BASE + HEADER_SIZE};
      .text : {{ {text_inputs} }}
      .rodata : {{ *(.rodata .rodata.* .srodata .srodata.*) }}
      {iram_output}
      .data {ram} : AT(__xip_data_load) {{
        __xip_data_start = .;
        *(.data .data.* .sdata .sdata.* .dram1 .dram1.* .tdata .tdata.*)
        *(.rtc_timer_data_in_rtc_mem) KEEP(*(.init_array .init_array.*))
        *(.xip_import_slots) *(.xip_vectors)
        . = ALIGN(16); __xip_data_end = .;
      }}
      .bss (NOLOAD) : {{
        __xip_bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(16); __xip_bss_end = .;
      }}
      /DISCARD/ : {{ *(.comment .note* .riscv.attributes) }}
      ASSERT(__xip_bss_end - __xip_data_start <= {RAM_SIZE}, "radio RAM arena overflow")
      ASSERT(SIZEOF(.s31_wifi_iram) <= {WIFI_IRAM_CAPACITY}, "Wi-Fi IRAM tail overflow")
    }}
    '''
    (out / 'radio-xip.lds').write_text(script)
    elf = out / 'radio-xip.elf'
    run(prefix + 'ld', '--no-relax', '-static', '-T', out / 'radio-xip.lds',
        '-Map=' + str(out / 'radio-xip.map'),
        *['--defsym=' + n + '=0' for n in nullable],
        '--defsym=_esp_err_msg_tbl_start=0', '--defsym=_esp_err_msg_tbl_end=0',
        out / 'radio-xip-imports.o', stripped, '-o', elf)
    image = read_elf(elf)
    image_symbols = read_symbols(image)
    sym = lambda name: image_symbols[name]['st_value']
    # Never silently omit a newly introduced loadable section.
    for sec in image.iter_sections():
        if not (sec['sh_flags'] & 2 and sec['sh_size']):
            continue
        if not (BASE + HEADER_SIZE <= sec['sh_addr'] <= sec['sh_addr'] + sec['sh_size'] <= BASE + SLOT_SIZE or
                ram <= sec['sh_addr'] <= sec['sh_addr'] + sec['sh_size'] <= ram + RAM_SIZE or
                WIFI_IRAM_BASE <= sec['sh_addr'] <= sec['sh_addr'] + sec['sh_size'] <=
                WIFI_IRAM_BASE + WIFI_IRAM_CAPACITY):
            raise ValueError('unexpected allocated section outside XIP/RAM contract')
        if sec['sh_flags'] & 1 and sec['sh_addr'] < ram:
            raise ValueError('writable allocated section in flash')
    raw = out / 'radio-xip-body.bin'
    run(prefix + 'objcopy', '-O', 'binary', elf, raw)
    body = raw.read_bytes()
    data_offset = sym('__xip_data_load') - BASE
    data_size = sym('__xip_data_end') - ram
    bss_size = sym('__xip_bss_end') - sym('__xip_bss_start')
    iram_offset = sym('__xip_iram_load') - BASE
    iram_size = sym('__xip_iram_end') - sym('__xip_iram_start')
    records = bytearray()
    for i, name in enumerate(functions):
        encoded = name.encode() + b'\0'
        if len(encoded) > 64:
            raise ValueError('import name exceeds image contract')
        records += struct.pack('<I64s', sym('__xip_import_slots') - ram + i * 4, encoded)
    exports = [sym(n) for n in EXPORTS]
    size = HEADER_SIZE + len(body)
    # Header CRC covers metadata (including body CRC) with header CRC zeroed.
    # No temporary full-image copy is needed on the device.
    header = bytearray(HEADER_SIZE)
    fields = [MAGIC, 2, 2, size, BASE, ram, RAM_SIZE, data_offset, data_size,
              sym('__xip_bss_start') - ram, bss_size,
              sym('_mtvt_table') - ram, len(functions), len(EXPORTS),
              zlib.crc32(body), 0]
    struct.pack_into('<16I', header, 0, *fields)
    struct.pack_into('<23I', header, 64, *exports)
    header[156:156 + len(records)] = records
    if 156 + len(records) > IRAM_FIELDS_OFFSET:
        raise ValueError('import table overlaps SRAM image fields')
    if not iram_size or iram_size > WIFI_IRAM_CAPACITY:
        raise ValueError('invalid Wi-Fi IRAM size')
    struct.pack_into('<2I', header, IRAM_FIELDS_OFFSET, iram_offset, iram_size)
    struct.pack_into('<I', header, 60, zlib.crc32(header))
    if size > SLOT_SIZE:
        raise ValueError('XIP image exceeds radio slot')
    args.output.write_bytes(header + body)
    report = dict(image_bytes=size, xip_bytes=data_offset - HEADER_SIZE,
                  ram_data_bytes=data_size, ram_bss_bytes=bss_size,
                  ram_capacity=RAM_SIZE, wifi_iram_bytes=iram_size,
                  wifi_iram_offset=iram_offset, imports=functions, nullable_imports=nullable,
                  exports=dict(zip(EXPORTS, exports)))
    import_obj = read_elf(out / 'radio-xip-imports.o')
    report['imports_iram_bytes'] = import_obj.get_section_by_name(imports_section)['sh_size']
    report['imports_iram_function_addresses'] = {n: sym(n) for n in functions}
    if selection:
        report['wifi_iram_requested'] = requested
        report['wifi_iram_sections'] = selection
        report['wifi_iram_function_addresses'] = {
            n: sym(n) for rec in selection for n in rec['functions'] if n in image_symbols}
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if not isinstance(v, (dict, list))}))

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prefix', required=True)
    p.add_argument('--kernel', required=True, type=Path)
    p.add_argument('--payload', required=True, type=Path)
    p.add_argument('--imports', required=True, type=Path)
    p.add_argument('--sram-symbols', required=True, type=Path, help='SRAM placement list from verified firmware inputs')
    p.add_argument('--output', required=True, type=Path)
    build(p.parse_args())

if __name__ == '__main__':
    main()
