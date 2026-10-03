#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Demote the radio closure's serialized picolibc errno TLS to module data."""

import os
import struct
import sys


SHT_RELA = 4
SHT_SYMTAB = 2
SHF_TLS = 0x400
STT_OBJECT = 1
STT_TLS = 6

R_RISCV_HI20 = 26
R_RISCV_LO12_I = 27
R_RISCV_LO12_S = 28
R_RISCV_TPREL_HI20 = 29
R_RISCV_TPREL_LO12_I = 30
R_RISCV_TPREL_LO12_S = 31
R_RISCV_TPREL_ADD = 32
R_RISCV_RELAX = 51

RELOC_MAP = {
    R_RISCV_TPREL_HI20: R_RISCV_HI20,
    R_RISCV_TPREL_LO12_I: R_RISCV_LO12_I,
    R_RISCV_TPREL_LO12_S: R_RISCV_LO12_S,
    # The Linux RISC-V module loader intentionally has no NONE entry, while
    # RELAX is a supported no-op relocation.
    R_RISCV_TPREL_ADD: R_RISCV_RELAX,
}


def fail(message):
    raise SystemExit(f"demote-riscv-tls: {message}")


def cstring(data, offset):
    end = data.find(b"\0", offset)
    if end < 0:
        fail("unterminated ELF string")
    return data[offset:end].decode("ascii", "strict")


def main(path):
    with open(path, "rb") as stream:
        data = bytearray(stream.read())

    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        fail(f"{path}: expected ELF32 little-endian input")

    shoff = struct.unpack_from("<I", data, 32)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 46)
    if shentsize != 40 or not shnum or shstrndx >= shnum:
        fail(f"{path}: unsupported section table")

    sections = []
    for index in range(shnum):
        offset = shoff + index * shentsize
        sections.append(list(struct.unpack_from("<10I", data, offset)))

    shstr = sections[shstrndx]
    shstr_data = data[shstr[4]:shstr[4] + shstr[5]]
    names = [cstring(shstr_data, section[0]) for section in sections]

    patched_relocs = 0
    errno_symbols = set()
    for rela_index, rela in enumerate(sections):
        if rela[1] != SHT_RELA:
            continue
        symtab_index, target_index = rela[6], rela[7]
        if symtab_index >= shnum or target_index >= shnum:
            fail(f"{path}: invalid relocation section links")
        symtab = sections[symtab_index]
        if symtab[1] != SHT_SYMTAB or symtab[9] != 16:
            fail(f"{path}: unsupported symbol table")
        strtab = sections[symtab[6]]
        strings = data[strtab[4]:strtab[4] + strtab[5]]

        for rel_off in range(rela[4], rela[4] + rela[5], 12):
            r_offset, r_info = struct.unpack_from("<II", data, rel_off)
            reloc_type = r_info & 0xFF
            symbol_index = r_info >> 8
            symbol_off = symtab[4] + symbol_index * symtab[9]
            symbol_name_off = struct.unpack_from("<I", data, symbol_off)[0]
            symbol_name = cstring(strings, symbol_name_off)

            # Newer ESP toolchains materialize errno as an STT_OBJECT with
            # ordinary HI20/LO12 relocations.  It can still reside in a TLS
            # section, but those relocations are already valid for a Linux
            # module; only that section needs demoting.  Older toolchains emit
            # TPREL relocations, which still need rewriting below.
            if reloc_type not in RELOC_MAP:
                if symbol_name == "errno":
                    errno_symbols.add((symtab_index, symbol_index))
                continue
            if symbol_name != "errno":
                fail(f"unsupported TLS symbol {symbol_name!r} in {names[rela_index]}")

            if reloc_type == R_RISCV_TPREL_ADD:
                target = sections[target_index]
                insn_off = target[4] + r_offset
                insn = struct.unpack_from("<I", data, insn_off)[0]
                if (insn & 0x7F) != 0x33 or ((insn >> 20) & 0x1F) != 4:
                    fail(f"unexpected TPREL_ADD instruction 0x{insn:08x}")
                rd = (insn >> 7) & 0x1F
                rs1 = (insn >> 15) & 0x1F
                addi = (rs1 << 15) | (rd << 7) | 0x13
                struct.pack_into("<I", data, insn_off, addi)

            new_info = (symbol_index << 8) | RELOC_MAP[reloc_type]
            struct.pack_into("<I", data, rel_off + 4, new_info)
            patched_relocs += 1
            errno_symbols.add((symtab_index, symbol_index))

    if not errno_symbols:
        fail(f"{path}: expected serialized errno TLS relocations")

    for symtab_index, symbol_index in errno_symbols:
        symtab = sections[symtab_index]
        symbol_off = symtab[4] + symbol_index * symtab[9]
        info = data[symbol_off + 12]
        section_index = struct.unpack_from("<H", data, symbol_off + 14)[0]
        if section_index >= shnum:
            fail(f"{path}: errno has an invalid section index")
        symbol_type = info & 0x0F
        if symbol_type not in (STT_TLS, STT_OBJECT):
            fail(f"{path}: errno has unsupported symbol type {symbol_type}")
        is_tls = bool(sections[section_index][2] & SHF_TLS)
        if symbol_type == STT_TLS and not is_tls:
            fail(f"{path}: STT_TLS errno is not in an SHF_TLS section")
        data[symbol_off + 12] = (info & 0xF0) | STT_OBJECT
        if is_tls:
            sections[section_index][2] &= ~SHF_TLS
            struct.pack_into("<I", data,
                             shoff + section_index * shentsize + 8,
                             sections[section_index][2])

    temporary = f"{path}.tls-demote.tmp"
    with open(temporary, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    print(f"demote-riscv-tls: converted {patched_relocs} errno relocations")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        fail("usage: demote_riscv_module_tls.py ELF")
    main(sys.argv[1])
