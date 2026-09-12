"""Run the actual SRAM flash guard against fake cache/flash ROM hooks."""
import ctypes
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
class FlashGuardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory()
        text=(ROOT/'opensbi-esp32-s31/platform/generic/espressif/esp32s31/services.c').read_text()
        start=text.rfind('static int __attribute__((section(".data.s31_flash_text"), noinline))',0,text.index('\ns31_flash_rom_operation('))
        source=text[start:text.index('static int s31_flash_ecall',start)]
        helper=text[text.index('#define S31_CSR_MHCR'):text.index('/* These words')]
        source=helper+source
        source=source.replace('__asm__ __volatile__("fence.i" ::: "memory");','')
        source=source.replace('__asm__ __volatile__("fence iorw, iorw" ::: "memory");','')
        definitions='\n'.join(l for l in text.splitlines() if l.startswith('#define S31_ROM_') and ('CACHE' in l or 'FLASH_' in l))
        hooks={'FLASH_WRITE':'write_rom','FLASH_ERASE':'erase_rom','CACHE_WRITEBACK_ALL':'wb','ICACHE0_SUSPEND':'s0','ICACHE1_SUSPEND':'s1','DCACHE_SUSPEND':'sd','ICACHE0_RESUME':'r0','ICACHE1_RESUME':'r1','DCACHE_RESUME':'rd'}
        for name,hook in hooks.items():
            definitions=re.sub(r'(#define S31_ROM_'+name+r')\s+0x[0-9a-f]+UL',r'\1 ((uintptr_t)'+hook+')',definitions)
        preamble=r'''#include <stdint.h>
#include <stdbool.h>
typedef uint32_t u32; typedef int32_t s32;
#define BIT(n) (1U<<(n))
#define S31_SBI_FLASH_ERASE 1
#define S31_SPI1_AUTO_RESUME_EN BIT(4)
#define S31_SPI1_AUTO_SUSPEND_EN BIT(5)
#define S31_PMU_STALL_CODE 0x86U
#define S31_PMU_STALL_TIMEOUT_CYCLES 100U
static u32 sus,stall,status,hart,fail,flashfail,cycles,mhcr,initial_mhcr,stallfail;
static char logbuf[32]; static int pos;
#define S31_SPI1_FLASH_SUS_CTRL ((uintptr_t)&sus)
#define S31_PMU_CPU_STALL_SW ((uintptr_t)&stall)
#define S31_HP_CORESTALLED_ST ((uintptr_t)&status)
static u32 current_hartid(void){return hart;}
static u32 s31_flash_rdcycle(void){if(stall && !stallfail)status=BIT(hart^1U);return cycles++;}
static void event(char c){logbuf[pos++]=c;logbuf[pos]=0;}
static u32 csr_read_clear(u32 csr,u32 bits){u32 saved=mhcr;mhcr &= ~bits;event('P');return saved;}
static void csr_set(u32 csr,u32 bits){mhcr |= bits;event('R');}
static int wb(u32 map){if(mhcr & (BIT(4)|BIT(5)|BIT(12)))return 99;event('W');if(fail)status=0;return fail;}
static u32 s0(void){event('0');return 7;}
static u32 s1(void){event('1');return 8;}
static u32 sd(void){event('D');return 9;}
static int write_rom(u32 addr,const u32 *buf,s32 len){event('F');return flashfail;}
static int erase_rom(u32 addr,u32 len){event('E');return flashfail;}
static void rd(u32 a){event(a==9?'d':'!');}
static void r0(u32 a){event(a==7?'a':'!');}
static void r1(u32 a){event(a==8?'b':'!');status=0;}
typedef int (*s31_rom_cache_writeback_all_t)(u32);
typedef u32 (*s31_rom_cache_suspend_t)(void);
typedef void (*s31_rom_cache_resume_t)(u32);
typedef int (*s31_rom_flash_write_t)(u32,const u32*,s32);
typedef int (*s31_rom_flash_erase_t)(u32,u32);
'''
        wrapper=r'''int invoke(u32 cpu,u32 func,u32 wb_fail,u32 flash_fail){
 mhcr=initial_mhcr=0x80000200 | (cpu ? BIT(5) : BIT(4)|BIT(12));
 hart=cpu;fail=wb_fail;flashfail=flash_fail;pos=0;cycles=0;status=0;stall=0;sus=0x12345678;
 return s31_flash_rom_operation(func,0xb30000,0,32);
}
int invoke_timeout(u32 cpu){
 mhcr=initial_mhcr=0x80000200 | (cpu ? BIT(5) : BIT(4)|BIT(12));
 hart=cpu;fail=flashfail=pos=cycles=status=stall=0;sus=0x12345678;stallfail=1;
 int ret=s31_flash_rom_operation(0,0xb30000,0,32);stallfail=0;return ret;
}
const char *events(void){return logbuf;}
u32 final_sus(void){return sus;}
u32 final_stall(void){return stall;}
int prediction_restored(void){return mhcr == initial_mhcr;}
'''
        p=Path(cls.tmp.name)/'guard.c';p.write_text(preamble+definitions+'\n'+source+wrapper)
        lib=p.with_suffix('.so')
        subprocess.run(['cc','-shared','-fPIC','-Wall','-Werror',str(p),'-o',str(lib)],check=True)
        cls.lib=ctypes.CDLL(str(lib));cls.lib.events.restype=ctypes.c_char_p
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def check_restore(self):
        self.assertEqual(self.lib.final_sus(),0x12345678)
        self.assertEqual(self.lib.final_stall(),0)
        self.assertEqual(self.lib.prediction_restored(),1)
    def test_write_and_erase_on_both_harts(self):
        for cpu in (0,1):
            for func,event in ((0,b'F'),(1,b'E')):
                self.assertEqual(self.lib.invoke(cpu,func,0,0),0)
                self.assertEqual(self.lib.events(),b'PW01D'+event+b'dabR');self.check_restore()
    def test_writeback_failure_aborts_before_cache_suspend(self):
        self.assertEqual(self.lib.invoke(0,0,1,0),1)
        self.assertEqual(self.lib.events(),b'PWR');self.check_restore()
    def test_flash_failure_still_restores_cache_and_peer(self):
        self.assertEqual(self.lib.invoke(1,0,0,3),3)
        self.assertEqual(self.lib.events(),b'PW01DFdabR');self.check_restore()

    def test_stall_timeout_restores_prediction_without_touching_caches(self):
        for cpu in (0,1):
            self.assertEqual(self.lib.invoke_timeout(cpu),2)
            self.assertEqual(self.lib.events(),b'PR');self.check_restore()
