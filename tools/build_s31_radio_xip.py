# SPDX-License-Identifier: BSD-2-Clause
"""Prelink the complete radio closure into an independently flashable XIP slot.

Code/constant relocations are resolved on the host. Module callbacks go through
fixed flash trampolines backed by writable import slots in the built-in arena.
The kernel's arena address is part of the image contract, not a runtime guess.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import zlib
MAGIC = 1479619411
BASE = 3189833728
HEADER_SIZE = 4096
SLOT_SIZE = 2031616
RAM_SIZE = 40960
EXPORTS = ('s31_radio_stack_task', 's31_radio_bt_enable_task', 's31_radio_bt_disable_task', 's31_radio_shutdown_task', 's31_radio_vhci_try_send', 's31_radio_coex_status', 's31_radio_wifi_scan_task', 's31_radio_wifi_connect_task', 's31_radio_wifi_disconnect_task', 's31_radio_wifi_control_task', 's31_radio_wifi_try_send_interface', 's31_radio_wifi_read_mac', 's31_radio_wifi_clock_enable', 's31_radio_wifi_clock_disable', 's31_radio_wifi_try_send', 's31_rtos_init', 's31_rtos_tick', 's31_linux_timer_next_due_us', 's31_rtos_hard_tick', 's31_rtos_free', 's31_rtos_task_release', 'xTaskCreatePinnedToCore', 's31_rtos_isr_depth')
EMPTY_CLOCK_IMPORTS = frozenset(('_esp_libc_clock_array_start', '_esp_libc_clock_array_end'))

class Elf:

    def __init__(self, path: Path):
        self.data = path.read_bytes()
        if self.data[:6] != b'\x7fELF\x01\x01':
            raise ValueError('expected little-endian ELF32')
        h = struct.unpack_from('<16sHHIIIIIHHHHHH', self.data)
        if h[2] != 243 or h[11] != 40:
            raise ValueError('expected RISC-V ELF32 section table')
        self.sections = [struct.unpack_from('<10I', self.data, h[6] + i * 40) for i in range(h[12])]
        strings = self.sections[h[13]]
        names = self.data[strings[4]:strings[4] + strings[5]]
        self.section_names = [names[sec[0]:names.find(b'\x00', sec[0])].decode() for sec in self.sections]
        self.symbols = {}
        for sec in self.sections:
            if sec[1] != 2:
                continue
            strings = self.sections[sec[6]]
            names = self.data[strings[4]:strings[4] + strings[5]]
            for pos in range(sec[4], sec[4] + sec[5], 16):
                n, v, size, info, other, idx = struct.unpack_from('<IIIBBH', self.data, pos)
                name = names[n:names.find(b'\x00', n)].decode()
                if name:
                    self.symbols[name] = (v, size, info, idx)

def run(*args):
    subprocess.run([str(a) for a in args], check=True)

def build(args):
    out = args.output.parent
    out.mkdir(parents=True, exist_ok=True)
    prefix = args.prefix
    kernel = Elf(args.kernel)
    ram = kernel.symbols['esp32s31_radio_xip_ram'][0]
    if kernel.symbols['esp32s31_radio_xip_ram'][1] != RAM_SIZE:
        raise ValueError('kernel XIP arena size differs')
    linked = {line.strip() for line in args.imports.read_text().splitlines() if line.strip() and (not line.startswith('#'))}
    stripped = out / 'radio-xip-input.o'
    run(prefix + 'objcopy', '--strip-unneeded', '--remove-section=.comment', *['--keep-symbol=' + name for name in (*EXPORTS, 's31_radio_fw_abi_version')], args.payload, stripped)
    payload = Elf(stripped)
    abi = payload.symbols['s31_radio_fw_abi_version']
    abi_section = payload.sections[abi[3]]
    if abi[1] != 4 or struct.unpack_from('<I', payload.data, abi_section[4] + abi[0])[0] != 1:
        raise ValueError('unsupported radio payload ABI')
    for name in EXPORTS[:-1]:
        if payload.symbols[name][2] & 15 != 2:
            raise ValueError('radio export is not a function: ' + name)
    undefined = sorted((n for n, s in payload.symbols.items() if s[3] == 0))
    functions = [n for n in undefined if n in linked and n not in ('_mtvt_table', '_esp_err_msg_tbl_start', '_esp_err_msg_tbl_end')]
    nullable = [n for n in undefined if n not in linked]
    for name in nullable:
        if name not in EMPTY_CLOCK_IMPORTS and payload.symbols[name][2] >> 4 != 2:
            raise ValueError('required import missing from binding contract: ' + name)
    for name in functions:
        if not re.fullmatch('[A-Za-z_][A-Za-z_0-9]*', name):
            raise ValueError('invalid import name')
    imports_section = '.text.xip_imports'
    asm = ['.option norelax', f'.section {imports_section},"ax",@progbits', '.balign 4']
    for i, name in enumerate(functions):
        asm += [f'.globl {name}', f'.type {name},@function', f'{name}:', f'lui t0,%hi(__xip_import_slots+{i * 4})', f'lw t0,%lo(__xip_import_slots+{i * 4})(t0)', 'jr t0']
    asm += ['.section .xip_import_slots,"aw",@progbits', '.balign 4', '.globl __xip_import_slots', '__xip_import_slots:', f'.zero {len(functions) * 4}', '.section .xip_vectors,"aw",@progbits', '.balign 4', '.globl _mtvt_table', '_mtvt_table:', '.zero 192']
    (out / 'radio-xip-imports.S').write_text('\n'.join(asm) + '\n')
    run(prefix + 'gcc', '-c', '-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f', out / 'radio-xip-imports.S', '-o', out / 'radio-xip-imports.o')
    text_inputs = '*(.text .text.* .iram1 .iram1.* .wifi*iram* .coexiram*)'
    iram_output = '. = ALIGN(16); __xip_data_load = .;'
    script = f'SECTIONS {{\n      . = {BASE + HEADER_SIZE};\n      .text : {{ {text_inputs} }}\n      .rodata : {{ *(.rodata .rodata.* .srodata .srodata.*) }}\n      {iram_output}\n      .data {ram} : AT(__xip_data_load) {{\n        __xip_data_start = .;\n        *(.data .data.* .sdata .sdata.* .dram1 .dram1.* .tdata .tdata.*)\n        *(.rtc_timer_data_in_rtc_mem) KEEP(*(.init_array .init_array.*))\n        *(.xip_import_slots) *(.xip_vectors)\n        . = ALIGN(16); __xip_data_end = .;\n      }}\n      .bss (NOLOAD) : {{\n        __xip_bss_start = .;\n        *(.bss .bss.* .sbss .sbss.* COMMON)\n        . = ALIGN(16); __xip_bss_end = .;\n      }}\n      /DISCARD/ : {{ *(.comment .note* .riscv.attributes) }}\n      ASSERT(__xip_bss_end - __xip_data_start <= {RAM_SIZE}, "radio RAM arena overflow")\n      \n    }}\n    '
    (out / 'radio-xip.lds').write_text(script)
    elf = out / 'radio-xip.elf'
    run(prefix + 'ld', '--no-relax', '-static', '-T', out / 'radio-xip.lds', '-Map=' + str(out / 'radio-xip.map'), *['--defsym=' + n + '=0' for n in nullable], '--defsym=_esp_err_msg_tbl_start=0', '--defsym=_esp_err_msg_tbl_end=0', out / 'radio-xip-imports.o', stripped, '-o', elf)
    image = Elf(elf)
    sym = lambda n: image.symbols[n][0]
    for sec in image.sections:
        if not (sec[2] & 2 and sec[5]):
            continue
        if not (BASE + HEADER_SIZE <= sec[3] <= sec[3] + sec[5] <= BASE + SLOT_SIZE or ram <= sec[3] <= sec[3] + sec[5] <= ram + RAM_SIZE):
            raise ValueError('unexpected allocated section outside XIP/RAM contract')
        if sec[2] & 1 and sec[3] < ram:
            raise ValueError('writable allocated section in flash')
    raw = out / 'radio-xip-body.bin'
    run(prefix + 'objcopy', '-O', 'binary', elf, raw)
    body = raw.read_bytes()
    data_offset = sym('__xip_data_load') - BASE
    data_size = sym('__xip_data_end') - ram
    bss_size = sym('__xip_bss_end') - sym('__xip_bss_start')
    iram_offset = 0
    iram_size = 0
    records = bytearray()
    for i, name in enumerate(functions):
        encoded = name.encode() + b'\x00'
        if len(encoded) > 64:
            raise ValueError('import name exceeds image contract')
        records += struct.pack('<I64s', sym('__xip_import_slots') - ram + i * 4, encoded)
    exports = [sym(n) for n in EXPORTS]
    size = HEADER_SIZE + len(body)
    header = bytearray(HEADER_SIZE)
    fields = [MAGIC, 1, 1, size, BASE, ram, RAM_SIZE, data_offset, data_size, sym('__xip_bss_start') - ram, bss_size, sym('_mtvt_table') - ram, len(functions), len(EXPORTS), zlib.crc32(body), 0]
    struct.pack_into('<16I', header, 0, *fields)
    struct.pack_into('<23I', header, 64, *exports)
    header[156:156 + len(records)] = records
    if 156 + len(records) > HEADER_SIZE:
        raise ValueError('import table exceeds header capacity')
    struct.pack_into('<I', header, 60, zlib.crc32(header))
    if size > SLOT_SIZE:
        raise ValueError('XIP image exceeds radio slot')
    args.output.write_bytes(header + body)
    report = dict(image_bytes=size, xip_bytes=data_offset - HEADER_SIZE, ram_data_bytes=data_size, ram_bss_bytes=bss_size, ram_capacity=RAM_SIZE, wifi_iram_bytes=iram_size, wifi_iram_offset=iram_offset, imports=functions, nullable_imports=nullable, exports=dict(zip(EXPORTS, exports)), sha256=hashlib.sha256(header + body).hexdigest())
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if not isinstance(v, (dict, list))}))

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prefix', required=True)
    p.add_argument('--kernel', required=True, type=Path)
    p.add_argument('--payload', required=True, type=Path)
    p.add_argument('--imports', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    build(p.parse_args())
if __name__ == '__main__':
    main()
