#!/usr/bin/env python3
"""Exercise the actual OpenSBI cache handler with a bounded fake ROM."""
import ctypes
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class CacheRomTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        source = (ROOT / "opensbi-esp32-s31/platform/generic/espressif/esp32s31/cache_mmu.c").read_text()
        source = re.sub(r'^#include .*$','',source,flags=re.M)
        for name, hook in (("INVALIDATE_ADDR", "mock_inval"), ("WRITEBACK_ADDR", "mock_wback"), ("INVALIDATE_ALL", "mock_all"), ("WRITEBACK_ALL", "mock_all")):
            source = re.sub(r'(#define S31_ROM_CACHE_'+name+r')\s+0x[0-9a-f]+UL',r'\1 ((unsigned long)'+hook+')',source)
        preamble = r'''
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define S31_FLASH_XIP_START 0x40000000U
#define S31_FLASH_SIZE 0x01000000U
#define S31_PSRAM_LINUX_START 0x50000000U
#define S31_PSRAM_LINUX_END 0x51000000U
#define SBI_ERR_INVALID_PARAM -3
#define SBI_ERR_FAILED -1
#define SBI_SUCCESS 0
#define SBI_ENOTSUPP -2
struct sbi_trap_regs { unsigned long a0, a1; };
struct sbi_ecall_return { unsigned long value; };
static u32 calls, last_addr, last_size, fail;
static int mock_inval(u32 map,u32 addr,u32 size) {
    ++calls; last_addr=addr; last_size=size;
    return fail || ((addr | size) & 63) || !size;
}
static int mock_wback(u32 map,u32 addr,u32 size) {return mock_inval(map,addr,size);}
static int mock_all(u32 map) {++calls;return fail;}
'''
        wrapper = r'''
long invoke(long func,u32 addr,u32 size,u32 error) {
    struct sbi_trap_regs regs={addr,size}; struct sbi_ecall_return out={0};
    calls=0;fail=error;return s31_cache_vendor_ext(func,&regs,&out);
}
u32 get_calls(void) {return calls;}
u32 get_addr(void) {return last_addr;}
u32 get_size(void) {return last_size;}
'''
        p=Path(cls.tmp.name)/"cache.c";p.write_text(preamble+source+wrapper)
        lib=p.with_suffix(".so")
        subprocess.run(["cc","-shared","-fPIC","-Wall","-Werror",str(p),"-o",str(lib)],check=True)
        cls.lib=ctypes.CDLL(str(lib));cls.lib.invoke.restype=ctypes.c_long
        cls.lib.invoke.argtypes=[ctypes.c_long,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_uint32]
    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()
    def test_short_unaligned_flash_ranges(self):
        for func in (0,1,2,4):
            for addr,size,expected in ((0x40a30001,4,64),(0x40a3003e,4,128),(0x40a30020,32,64),(0x40ffffff,1,64)):
                self.assertEqual(self.lib.invoke(func,addr,size,0),0)
                self.assertEqual(self.lib.get_addr(),addr & ~63)
                self.assertEqual(self.lib.get_size(),expected)
    def test_invalid_ranges_never_reach_rom(self):
        for addr,size in ((0x40000000,0),(0x3fffffff,1),(0x40ffffff,2),(0x50ffffff,2),(0x50000000,0xffffffff)):
            self.assertEqual(self.lib.invoke(1,addr,size,0),-3)
            self.assertEqual(self.lib.get_calls(),0)
    def test_rom_failures_propagate_and_stop_chained_operations(self):
        for func in range(6):
            self.assertEqual(self.lib.invoke(func,0x50000001,4,1),-1)
            self.assertEqual(self.lib.get_calls(),1)
    def test_unknown_operation(self):
        self.assertEqual(self.lib.invoke(99,0x50000000,64,0),-2)
        self.assertEqual(self.lib.get_calls(),0)

if __name__ == "__main__":
    unittest.main()
