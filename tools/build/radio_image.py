#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Prelink the complete radio closure into an independently flashable XIP slot.

Code/constant relocations are resolved on the host. Module callbacks go through
fixed SRAM trampolines backed by writable import slots in the built-in arena.
The kernel's arena address is part of the image contract, not a runtime guess.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import zlib
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.build.toolchain_identity import identify as toolchain_identity

MAGIC = 0x58313353  # S31X
BASE = 0xBE06E000
HEADER_SIZE = 4096
SLOT_SIZE = 0x180000
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
    's31_linux_timer_next_due_us', 's31_rtos_hard_tick', 's31_rtos_free',
    's31_rtos_task_release', 'xTaskCreatePinnedToCore', 's31_rtos_isr_depth',
)
EMPTY_CLOCK_IMPORTS = frozenset(('_esp_libc_clock_array_start', '_esp_libc_clock_array_end'))


class Elf:
    def __init__(self, path: Path):
        self.data = path.read_bytes()
        if self.data[:6] != b'\x7fELF\x01\x01':
            raise ValueError('expected little-endian ELF32')
        h = struct.unpack_from('<16sHHIIIIIHHHHHH', self.data)
        if h[2] != 243 or h[11] != 40:
            raise ValueError('expected RISC-V ELF32 section table')
        self.sections = [struct.unpack_from('<10I', self.data, h[6] + i * 40)
                         for i in range(h[12])]
        strings = self.sections[h[13]]
        names = self.data[strings[4]:strings[4] + strings[5]]
        self.section_names = [names[sec[0]:names.find(b"\0", sec[0])].decode()
                              for sec in self.sections]
        self.symbols = {}
        for sec in self.sections:
            if sec[1] != 2:
                continue
            strings = self.sections[sec[6]]
            names = self.data[strings[4]:strings[4] + strings[5]]
            for pos in range(sec[4], sec[4] + sec[5], 16):
                n, v, size, info, other, idx = struct.unpack_from('<IIIBBH', self.data, pos)
                name = names[n:names.find(b'\0', n)].decode()
                if name:
                    self.symbols[name] = (v, size, info, idx)


def select_iram_sections(payload, requested):
    """Select whole executable input sections; report co-resident functions too."""
    selected = set()
    for name in requested:
        if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9.]*', name):
            raise ValueError('invalid SRAM function name: ' + name)
        sym = payload.symbols.get(name)
        if sym is None or sym[2] & 15 != 2 or not 0 < sym[3] < len(payload.sections):
            raise ValueError('SRAM target is not a defined payload function: ' + name)
        sec = payload.sections[sym[3]]
        section_name = payload.section_names[sym[3]]
        if (sec[1] != 1 or sec[2] & 7 != 6 or not sec[5] or
                not re.fullmatch(r'\.[A-Za-z_0-9.]+', section_name)):
            raise ValueError('SRAM target section is not read-only executable code: ' + name)
        selected.add(sym[3])
    if not selected:
        raise ValueError('empty SRAM function selection')
    names = [payload.section_names[i] for i in sorted(selected)]
    # objcopy renames by name, so ambiguous section names would move extra code.
    if any(payload.section_names.count(n) != 1 for n in names):
        raise ValueError('ambiguous SRAM input section name')
    report = []
    for idx in sorted(selected):
        report.append(dict(section=payload.section_names[idx], bytes=payload.sections[idx][5],
                           functions=sorted(n for n, sym in payload.symbols.items()
                                            if sym[3] == idx and sym[2] & 15 == 2)))
    return report


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def input_identity(args):
    root = Path(__file__).resolve().parents[2]
    files = {'kernel_vmlinux': args.kernel, 'kernel_module': args.module,
             'radio_payload': args.payload, 'radio_imports': args.imports,
             'builder': Path(__file__), 'toolchain_identity': root / 'tools/build/toolchain_identity.py', 'layout': root / 'configs/esp32s31-layout.cfg',
             'wifi_sram_symbols': root / 'firmware/radio/wifi_sram_symbols.txt'}
    for tool in ('gcc', 'ld', 'objcopy'):
        executable = shutil.which(args.prefix + tool)
        if executable is None:
            raise ValueError('missing radio build tool: ' + args.prefix + tool)
        files['tool_' + tool] = Path(executable)
    identity = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in files.items()}
    identity['toolchain'] = toolchain_identity(files['tool_gcc'])['sha256']
    return identity


def build(args):
    identity = input_identity(args)
    report_path = args.output.with_suffix('.json')
    if args.output.is_file() and report_path.is_file():
        try:
            previous = json.loads(report_path.read_text())
            if (previous.get('build_inputs') == identity and
                    previous.get('binding') == {name + '_sha256': identity[name] for name in
                        ('kernel_vmlinux', 'kernel_module', 'radio_payload', 'radio_imports')} and
                    previous.get('sha256') == hashlib.sha256(args.output.read_bytes()).hexdigest()):
                print('Radio image inputs unchanged: ' + str(args.output))
                return
        except (ValueError, OSError):
            pass
    out = args.output.parent
    out.mkdir(parents=True, exist_ok=True)
    prefix = args.prefix
    kernel = Elf(args.kernel)
    ram = kernel.symbols['esp32s31_radio_xip_ram'][0]
    if kernel.symbols['esp32s31_radio_xip_ram'][1] != RAM_SIZE:
        raise ValueError('kernel XIP arena size differs')
    linked = {line.strip() for line in args.imports.read_text().splitlines()
              if line.strip() and not line.startswith('#')}
    # Fixed production placement; missing targets fail rather than fall back to flash.
    manifest = Path(__file__).resolve().parents[2] / 'firmware/radio/wifi_sram_symbols.txt'
    requested = sorted(set(line.split('#', 1)[0].strip()
                           for line in manifest.read_text().splitlines()) - {''})
    selection = select_iram_sections(Elf(args.payload), requested)
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
    payload = Elf(stripped)
    abi = payload.symbols['s31_radio_fw_abi_version']
    abi_section = payload.sections[abi[3]]
    if abi[1] != 4 or struct.unpack_from('<I', payload.data,
            abi_section[4] + abi[0])[0] != 1:
        raise ValueError('unsupported radio payload ABI')
    for name in EXPORTS[:-1]:
        if payload.symbols[name][2] & 15 != 2:
            raise ValueError('radio export is not a function: ' + name)
    undefined = sorted(n for n, s in payload.symbols.items() if s[3] == 0)
    functions = [n for n in undefined if n in linked and
                 n not in ('_mtvt_table', '_esp_err_msg_tbl_start', '_esp_err_msg_tbl_end')]
    nullable = [n for n in undefined if n not in linked]
    for name in nullable:
        if name not in EMPTY_CLOCK_IMPORTS and payload.symbols[name][2] >> 4 != 2:
            raise ValueError('required import missing from binding contract: ' + name)
    for name in functions:
        if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name):
            raise ValueError('invalid import name')
    imports_section = '.s31_tx_iram.imports'
    asm = ['.option norelax', f'.section {imports_section},"ax",@progbits', '.balign 4']
    for i, name in enumerate(functions):
        asm += [f'.globl {name}', f'.type {name},@function', f'{name}:',
                f'lui t0,%hi(__xip_import_slots+{i * 4})',
                f'lw t0,%lo(__xip_import_slots+{i * 4})(t0)', 'jr t0']
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
    image = Elf(elf)
    sym = lambda n: image.symbols[n][0]
    # Never silently omit a newly introduced loadable section.
    for sec in image.sections:
        if not (sec[2] & 2 and sec[5]):
            continue
        if not (BASE + HEADER_SIZE <= sec[3] <= sec[3] + sec[5] <= BASE + SLOT_SIZE or
                ram <= sec[3] <= sec[3] + sec[5] <= ram + RAM_SIZE or
                WIFI_IRAM_BASE <= sec[3] <= sec[3] + sec[5] <=
                WIFI_IRAM_BASE + WIFI_IRAM_CAPACITY):
            raise ValueError('unexpected allocated section outside XIP/RAM contract')
        if sec[2] & 1 and sec[3] < ram:
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
    fields = [MAGIC, 2, 1, size, BASE, ram, RAM_SIZE, data_offset, data_size,
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
                  exports=dict(zip(EXPORTS, exports)),
                  sha256=hashlib.sha256(header + body).hexdigest(),
                  build_inputs=identity,
                  binding={
                      'kernel_vmlinux_sha256': identity['kernel_vmlinux'],
                      'kernel_module_sha256': identity['kernel_module'],
                      'radio_payload_sha256': identity['radio_payload'],
                      'radio_imports_sha256': identity['radio_imports'],
                  })
    import_obj = Elf(out / 'radio-xip-imports.o')
    idx = import_obj.section_names.index(imports_section)
    report['imports_iram_bytes'] = import_obj.sections[idx][5]
    report['imports_iram_function_addresses'] = {n: sym(n) for n in functions}
    if selection:
        report['wifi_iram_requested'] = requested
        report['wifi_iram_sections'] = selection
        report['wifi_iram_function_addresses'] = {
            n: sym(n) for rec in selection for n in rec['functions'] if n in image.symbols}
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if not isinstance(v, (dict, list))}))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prefix', required=True)
    p.add_argument('--kernel', required=True, type=Path)
    p.add_argument('--module', required=True, type=Path)
    p.add_argument('--payload', required=True, type=Path)
    p.add_argument('--imports', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    build(p.parse_args())


if __name__ == '__main__':
    main()
