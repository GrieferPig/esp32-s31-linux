#!/usr/bin/env python3
"""Run the kernel loader's pure ELF preflight on valid and malformed inputs."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def fixture():
    data = bytearray(400)
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<16sHHIIIIIHHHHHH", data, 0, ident, 1, 243, 1, 0, 0, 200, 0, 52, 0, 0, 40, 5, 0)
    data[52:60] = bytes(8)  # alloc/executable section
    data[60:66] = b"\0name\0"
    struct.pack_into("<IIIBBH", data, 96, 1, 0, 8, 0x12, 0, 1)
    struct.pack_into("<IIi", data, 112, 0, (1 << 8) | 18, 0)  # 8-byte CALL
    sections = [
        (0,) * 10,
        (0, 1, 6, 0, 52, 8, 0, 0, 4, 0),
        (0, 3, 0, 0, 60, 6, 0, 0, 1, 0),
        (0, 2, 0, 0, 80, 32, 2, 1, 4, 16),
        (0, 4, 0, 0, 112, 12, 3, 1, 4, 12),
    ]
    for i, s in enumerate(sections):
        struct.pack_into("<10I", data, 200 + 40 * i, *s)
    return data


class RadioElf(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="s31-elf-test-")
        cls.work = Path(cls.temp.name)
        source = r"""
#include <elf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef uint32_t u32;
#include "esp32s31-radio-elf.h"
int main(int argc, char **argv) {
    if (argc == 1) {
        char bad[4] = {'A', 'A', 'A', 'A'};
        Elf32_Shdr s[2] = {{0}, {.sh_flags=SHF_ALLOC|SHF_EXECINSTR, .sh_addr=0x1000, .sh_size=8}};
        Elf32_Sym sym = {.st_shndx=1, .st_value=0x1000};
        if (s31_fw_string_valid(bad, 4, 1)) return 1;
        if (!s31_fw_export_valid(&sym, s, 2, true, false)) return 2;
        sym.st_value=0x1008;
        if (s31_fw_export_valid(&sym, s, 2, true, false)) return 3;
        sym.st_value=0x1000; sym.st_shndx=SHN_ABS;
        if (s31_fw_export_valid(&sym, s, 2, false, false)) return 4;
        sym.st_shndx=1;
        if (s31_fw_export_valid(&sym, s, 2, false, true)) return 5;
        return 0;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long n=ftell(f); rewind(f);
    void *data=malloc(n ? n : 1);
    if (!data || fread(data, 1, n, f) != (size_t)n) return 2;
    fclose(f);
    int result=s31_fw_validate_elf(data, n);
    free(data);
    if (result) fprintf(stderr, "invalid ELF: %d\n", result);
    return result ? 1 : 0;
}
"""
        (cls.work / "validate.c").write_text(source)
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("S31_TEST_SANITIZERS") else []
        subprocess.run(["cc", "-O1", "-g", "-Wall", "-Werror", *flags,
                        "-I" + str(ROOT / "linux-esp32-s31/drivers/platform"),
                        str(cls.work / "validate.c"), "-o", str(cls.work / "validate")], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check_elf(self, data, valid):
        (self.work / "input.o").write_bytes(data)
        p = subprocess.run([str(self.work / "validate"), str(self.work / "input.o")], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0 if valid else 1, p.stderr)
        self.assertNotIn("Sanitizer", p.stderr)

    def test_valid_elf_and_export_bounds(self):
        self.check_elf(fixture(), True)
        subprocess.run([str(self.work / "validate")], check=True)

    def test_malformed_elf(self):
        cases = {
            "bad class": (4, "B", 2),
            "section table truncated": (32, "I", 392),
            "section alignment overflow": (240 + 32, "I", 0x80000000),
            "image size overflow": (240 + 20, "I", 0xffffffff),
            "section outside file": (240 + 16, "I", 399),
            "missing string terminator": (65, "B", 65),
            "symbol name out of bounds": (96, "I", 6),
            "symbol section out of bounds": (96 + 14, "H", 8),
            "symbol value beyond section": (96 + 4, "I", 9),
            "symbol size beyond section": (96 + 8, "I", 9),
            "string table wrong type": (280 + 4, "I", 8),
            "allocated symbol metadata": (320 + 8, "I", 2),
            "allocated relocation metadata": (360 + 8, "I", 2),
            "symbol entry size": (320 + 36, "I", 12),
            "symbol table remainder": (320 + 20, "I", 31),
            "relocation symbol": (116, "I", (3 << 8) | 18),
            "call crosses end": (112, "I", 4),
            "unknown relocation": (116, "I", (1 << 8) | 255),
            "unbounded uleb relocation": (116, "I", (1 << 8) | 60),
            "relocation sh_link": (360 + 24, "I", 2),
            "relocation target section": (360 + 28, "I", 9),
            "relocation entry size": (360 + 36, "I", 8),
            "relocation remainder": (360 + 20, "I", 11),
            "relocation alignment": (360 + 16, "I", 113),
        }
        for name, (offset, fmt, value) in cases.items():
            with self.subTest(name=name):
                data = fixture()
                struct.pack_into("<" + fmt, data, offset, value)
                self.check_elf(data, False)
        for length in (0, 4, 51, 199, 399):
            with self.subTest(truncated=length):
                self.check_elf(fixture()[:length], False)

    def test_existing_payload_when_available(self):
        p = ROOT / "build/esp32s31-radio-fw-v1.o"
        if not p.is_file():
            self.skipTest("optional real payload not built; synthetic valid ELF always tested")
        self.check_elf(p.read_bytes(), True)


if __name__ == "__main__":
    unittest.main()
