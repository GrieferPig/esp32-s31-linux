#!/usr/bin/env python3
"""Validate the S31 ELF -> raw -> FIT -> merged flash address contract."""
import argparse
import json
from pathlib import Path
import struct


def require(ok, message):
    if not ok:
        raise ValueError(message)


def span(data, offset, size):
    require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset,
            'truncated ELF/FDT data')
    return data[offset:offset + size]


def cstring(data, offset=0):
    end = data.find(b'\0', offset)
    require(0 <= offset <= end, 'unterminated string')
    return data[offset:end].decode('ascii')


def elf_layout(data):
    require(data[:6] == b'\x7fELF\x01\x01', 'expected little-endian ELF32')
    h = struct.unpack('<16sHHIIIIIHHHHHH', span(data, 0, 52))
    require(h[2] == 243 and h[9] == 32 and h[11] == 40, 'expected RISC-V ELF32 tables')
    ph = [struct.unpack('<8I', span(data, h[5] + i * 32, 32)) for i in range(h[10])]
    sh = [struct.unpack('<10I', span(data, h[6] + i * 40, 40)) for i in range(h[12])]
    require(h[13] < len(sh), 'invalid section names')
    names = span(data, sh[h[13]][4], sh[h[13]][5])
    sections, symbols = [], {}
    for s in sh:
        if s[1] == 2:
            require(s[6] < len(sh) and s[9] == 16 and s[5] % 16 == 0, 'invalid symbol table')
            strings = span(data, sh[s[6]][4], sh[s[6]][5])
            for pos in range(s[4], s[4] + s[5], 16):
                n, value, size, info, other, index = struct.unpack('<IIIBBH', span(data, pos, 16))
                if n and index:
                    symbols[cstring(strings, n)] = value
        if not (s[2] & 2) or s[1] == 8 or not s[5]:
            continue
        candidates = [p for p in ph if p[0] == 1 and p[1] <= s[4] and s[4] + s[5] <= p[1] + p[4]]
        require(len(candidates) == 1, 'allocated section lacks unambiguous load segment')
        p = candidates[0]
        sections.append({'name': cstring(names, s[0]), 'vma': s[3],
                         'lma': p[3] + s[4] - p[1], 'size': s[5],
                         'bytes': span(data, s[4], s[5])})
    require(sections, 'no loadable sections')
    return h[4], sections, symbols


def fdt_props(data):
    h = struct.unpack('>10I', span(data, 0, 40))
    require(h[0] == 0xd00dfeed and h[1] <= len(data), 'invalid FIT/FDT header')
    strings = span(data, h[3], h[8])
    block = span(data, h[2], h[9])
    pos, nodes, props = 0, [], {}
    while pos < len(block):
        token = struct.unpack('>I', span(block, pos, 4))[0]
        pos += 4
        if token == 1:
            name = cstring(block, pos)
            nodes.append(name)
            pos = (pos + len(name) + 1 + 3) & ~3
        elif token == 2:
            require(nodes, 'unbalanced FDT node')
            nodes.pop()
        elif token == 3:
            size, name = struct.unpack('>II', span(block, pos, 8))
            pos += 8
            props[('/' + '/'.join(n for n in nodes if n), cstring(strings, name))] = span(block, pos, size)
            pos = (pos + size + 3) & ~3
        elif token == 9:
            require(not nodes, 'unclosed FDT node')
            return h[1], props
        else:
            require(token == 4, 'invalid FDT token')
    raise ValueError('FDT lacks END token')


def number(value):
    require(len(value) in (4, 8), 'expected one or two FDT cells')
    return int.from_bytes(value, 'big')


def fit_payload(data, props, totalsize, name):
    node = '/images/' + name
    require(props.get((node, 'compression'), b'none\0') == b'none\0', 'compressed FIT firmware unsupported')
    size = number(props[(node, 'data-size')])
    if (node, 'data-position') in props:
        offset = number(props[(node, 'data-position')])
    else:
        offset = ((totalsize + 3) & ~3) + number(props[(node, 'data-offset')])
    return offset, span(data, offset, size)


def check(elf, raw, fit=None, flash=None, text_base=0x4000e400, rw_base=0x2f00f000,
          fit_base=0x4000e000, fit_offset=0xe000):
    entry, sections, symbols = elf_layout(elf)
    first = min(s['lma'] for s in sections)
    require(entry == symbols['_start'] == symbols['_fw_start'] == first == text_base,
            f'entry/raw/link mismatch: entry={entry:#x} _start={symbols["_start"]:#x} '
            f'_fw_start={symbols["_fw_start"]:#x} first LMA={first:#x} FIT load={text_base:#x}')
    require(symbols['_data_start'] == rw_base, 'OpenSBI writable base differs from SRAM contract')
    require(rw_base < symbols['_fw_end'] <= rw_base + 0x3000, 'OpenSBI static SRAM allocation exceeds reserved image budget')
    end = max(s['lma'] + s['size'] for s in sections)
    require(len(raw) == end - first, 'raw binary size differs from ELF load span')
    for s in sections:
        require(span(raw, s['lma'] - first, s['size']) == s['bytes'], 'raw bytes differ for ELF section ' + s['name'])
    result = dict(entry=hex(entry), raw_base=hex(first), raw_bytes=len(raw),
                  rw_base=hex(rw_base), static_rw_end=hex(symbols['_fw_end']))
    if fit is not None:
        total, props = fdt_props(fit)
        config = cstring(props[('/configurations', 'default')])
        config_node = '/configurations/' + config
        firmware = cstring(props[(config_node, 'firmware')])
        load = number(props[('/images/' + firmware, 'load')])
        offset, payload = fit_payload(fit, props, total, firmware)
        require(load == text_base == fit_base + offset, 'FIT load and physical external-data placement disagree')
        require(payload == raw, 'FIT carries a different OpenSBI binary')
        if ('/images/' + firmware, 'entry') in props:
            require(number(props[('/images/' + firmware, 'entry')]) == entry, 'FIT entry differs from ELF')
        fdt_name = cstring(props[(config_node, 'fdt')])
        _, dtb = fit_payload(fit, props, total, fdt_name)
        _, dtprops = fdt_props(dtb)
        heap = number(dtprops[('/chosen/opensbi-config', 'heap-size')])
        # The matched S31 platform fixes two harts and 4-KiB stacks. The
        # radio uses SBI function 6 to exclude this full dynamic footprint.
        footprint = symbols['_fw_end'] + 2 * 4096 + heap
        require(footprint <= 0x2f030000, 'OpenSBI stacks/heap overlap the fixed radio arena')
        result.update(fit_data_offset=hex(offset), heap_bytes=heap,
                      complete_rw_end=hex(footprint), low_radio_heap_start=hex(max(0x2f018000, footprint)))
        if flash is not None:
            require(span(flash, fit_offset, len(fit)) == fit, 'merged flash contains a different FIT')
            result['flash_fit_offset'] = hex(fit_offset)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--elf', type=Path, required=True)
    p.add_argument('--raw', type=Path, required=True)
    p.add_argument('--fit', type=Path)
    p.add_argument('--flash', type=Path)
    a = p.parse_args()
    try:
        print(json.dumps(check(a.elf.read_bytes(), a.raw.read_bytes(),
                               a.fit.read_bytes() if a.fit else None,
                               a.flash.read_bytes() if a.flash else None), indent=2))
    except (ValueError, KeyError, OSError, struct.error) as e:
        p.exit(1, 'OpenSBI image contract: ' + str(e) + '\n')


if __name__ == '__main__':
    main()
