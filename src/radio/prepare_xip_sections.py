#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Name each IRAM input section after a relocatable --unique link.

ld -r normally merges identically named sections from unrelated archive members.
--unique retains them, but a later ld -r would merge them again. Only section
names/string-table locations change here: code, relocations and symbol section
indices remain untouched. This does not split already coalesced sections.
"""
import argparse
from pathlib import Path
import re
import struct


def uniquify(data):
    if len(data) < 52 or data[:6] != b'\x7fELF\x01\x01':
        raise ValueError('expected little-endian ELF32 relocatable object')
    h = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    if h[1] != 1 or h[2] != 243 or h[10] or h[11] != 40 or not 0 < h[13] < h[12]:
        raise ValueError('expected RISC-V ELF32 relocatable section table')
    if h[6] + h[12] * 40 > len(data):
        raise ValueError('truncated ELF section table')
    sections = [struct.unpack_from('<10I', data, h[6] + i * 40) for i in range(h[12])]
    strings = sections[h[13]]
    if strings[1] != 3 or strings[4] + strings[5] > len(data):
        raise ValueError('invalid section name table')
    original_names = data[strings[4]:strings[4] + strings[5]]
    names = bytearray(original_names)
    renamed = []
    for i, sec in enumerate(sections):
        if sec[1] != 8 and sec[4] + sec[5] > len(data):
            raise ValueError('truncated ELF section contents')
        if sec[0] >= len(original_names) or original_names.find(b'\0', sec[0]) == -1:
            raise ValueError('invalid section name')
        name = original_names[sec[0]:original_names.find(b'\0', sec[0])].decode()
        if (sec[1] == 1 and sec[2] & 7 == 6 and
                (name.startswith('.iram1') or name.startswith('.wifi') and 'iram' in name) and
                not re.search(r'\.s31_input_\d+$', name)):
            renamed.append((i, len(names)))
            names.extend((name + '.s31_input_' + str(i)).encode() + b'\0')
    if not renamed:
        return data
    result = bytearray(data)
    offset = len(result)
    result.extend(names)
    for idx, name in renamed:
        struct.pack_into('<I', result, h[6] + idx * 40, name)
    struct.pack_into('<II', result, h[6] + h[13] * 40 + 16, offset, len(names))
    return bytes(result)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('object', type=Path)
    args = p.parse_args()
    original = args.object.read_bytes()
    result = uniquify(original)
    if result != original:
        temporary = args.object.with_suffix(args.object.suffix + '.iram-tmp')
        temporary.write_bytes(result)
        temporary.replace(args.object)
    print('unique IRAM names:', args.object, len(original), '->', len(result))


if __name__ == '__main__':
    main()