#!/usr/bin/env python3
"""Exercise the current radio XIP header preflight against malformed layouts."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RadioXipHeader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="s31-xip-test-")
        cls.work = Path(cls.temp.name)
        source = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint8_t u8;
#include "esp32s31-radio-xip.h"

#define RAM_BASE 0x10000U
static void valid_header(struct s31_xip_header *h)
{
    unsigned int i;
    memset(h, 0, sizeof(*h));
    h->magic = S31_XIP_MAGIC;
    h->version = 2;
    h->abi = 1;
    h->image_size = 12288;
    h->base = S31_XIP_BASE;
    h->ram = RAM_BASE;
    h->ram_capacity = S31_XIP_RAM_SIZE;
    h->data_offset = 8192;
    h->data_size = 256;
    h->bss_offset = 256;
    h->bss_size = 64;
    h->vectors_offset = 0;
    h->export_count = S31_XIP_EXPORTS;
    h->wifi_iram_offset = 4096;
    h->wifi_iram_size = 64;
    for (i = 0; i < S31_XIP_EXPORTS - 1; i++)
        h->exports[i] = S31_XIP_BASE + 5000;
    h->exports[22] = RAM_BASE + 256;
}
#define INVALID(change) do { valid_header(&h); change; if (s31_xip_valid(&h, RAM_BASE)) { fprintf(stderr, "unexpectedly accepted: %s\\n", #change); return __LINE__; } } while (0)
int main(void)
{
    struct s31_xip_header h;
    valid_header(&h);
    if (!s31_xip_valid(&h, RAM_BASE)) return 1;
    if (s31_xip_valid(&h, RAM_BASE + 4)) return 2;
    INVALID(h.magic = 0);
    INVALID(h.version = 1);
    INVALID(h.abi = 2);
    INVALID(h.image_size = S31_XIP_HEADER_SIZE);
    INVALID(h.image_size = S31_XIP_SLOT_SIZE + 1);
    INVALID(h.base++);
    INVALID(h.ram_capacity++);
    INVALID(h.export_count--);
    INVALID(h.import_count = S31_XIP_MAX_IMPORTS + 1);
    INVALID(h.data_offset = S31_XIP_HEADER_SIZE - 1);
    INVALID(h.data_offset = h.image_size);
    INVALID(h.bss_offset++);
    INVALID(h.vectors_offset = h.data_size - 191);
    INVALID(h.wifi_iram_size = 0);
    INVALID(h.wifi_iram_size = S31_XIP_WIFI_IRAM_CAPACITY + 1);
    INVALID(h.wifi_iram_offset = h.data_offset);
    INVALID(h.exports[0] |= 1);
    INVALID(h.exports[0] = S31_XIP_BASE + h.data_offset);
    INVALID(h.exports[22] = RAM_BASE + h.bss_offset + h.bss_size);
    valid_header(&h);
    h.import_count = 1;
    h.imports[0].offset = 4;
    strcpy(h.imports[0].name, "esp32s31_radio_import");
    if (!s31_xip_valid(&h, RAM_BASE)) return 3;
    INVALID(h.import_count = 1; h.imports[0].offset = h.data_size; strcpy(h.imports[0].name, "symbol"));
    INVALID(h.import_count = 1; h.imports[0].offset = 1; strcpy(h.imports[0].name, "symbol"));
    INVALID(h.import_count = 1; h.imports[0].offset = 4; strcpy(h.imports[0].name, "symbol"); h.imports[0].name[0] = 0);
    valid_header(&h);
    h.import_count = 2;
    h.imports[0].offset = 4;
    h.imports[1].offset = 8;
    strcpy(h.imports[0].name, "same");
    strcpy(h.imports[1].name, "same");
    if (s31_xip_valid(&h, RAM_BASE)) return 4;
    return 0;
}
"""
        (cls.work / "validate.c").write_text(source)
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("S31_TEST_SANITIZERS") else []
        subprocess.run([
            "cc", "-O1", "-g", "-Wall", "-Werror", *flags,
            "-I" + str(ROOT / "linux-esp32-s31/drivers/platform"),
            str(cls.work / "validate.c"), "-o", str(cls.work / "validate")
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_valid_and_malformed_xip_headers(self):
        subprocess.run([str(self.work / "validate")], check=True)


if __name__ == "__main__":
    unittest.main()
