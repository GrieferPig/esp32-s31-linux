"""Exercise the actual SRAM park protocol, including late cancellation."""
import ctypes
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time
import unittest
ROOT=Path(__file__).resolve().parents[2]

class SramPeerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory()
        text=(ROOT/'opensbi-esp32-s31/platform/generic/espressif/esp32s31/services.c').read_text()
        source=text[text.index('#define S31_CSR_MHCR'):text.index('static inline u32 s31_flash_rdcycle')]
        source=source.replace('__asm__ __volatile__("fence.i" ::: "memory");','')
        source=source.replace('__asm__ __volatile__("fence iorw, iorw" ::: "memory");','')
        defines='\n'.join(l for l in text.splitlines() if l.startswith('#define S31_SBI_FLASH_'))
        preamble=r'''#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
#define SBI_SUCCESS 0
#define SBI_ERR_INVALID_PARAM -3
#define SBI_ERR_ALREADY_AVAILABLE -6
#define SBI_ERR_NOT_SUPPORTED -2
struct sbi_trap_regs {unsigned long a0,a1;};
struct sbi_ecall_return {unsigned long value;};
#define BIT(n) (1U << (n))
static _Thread_local u32 cpu;
static _Thread_local u32 mhcr=0x80001230;
static volatile u32 prediction[2];
static u32 csr_read_clear(u32 csr,u32 bits){u32 saved=mhcr;mhcr &= ~bits;prediction[cpu]=mhcr;return saved;}
static void csr_set(u32 csr,u32 bits){mhcr |= bits;prediction[cpu]=mhcr;}
static u32 current_hartid(void){return cpu;}
'''
        wrapper=r'''long control(u32 func,u32 peer,u32 queued,u32 hart){
 struct sbi_trap_regs regs={peer,queued};struct sbi_ecall_return out={0};cpu=hart;
 int ret=s31_flash_peer_control(func,&regs,&out);return ret?ret:out.value;
}
void reset(void){memset(s31_flash_peers,0,sizeof(s31_flash_peers));prediction[0]=prediction[1]=mhcr=0x80001230;}
u32 prediction_state(u32 peer){return prediction[peer];}
u32 raw_status(u32 peer){return s31_flash_peers[peer].entered|(s31_flash_peers[peer].exited<<1);}
void force_release(u32 peer){s31_flash_peers[peer].release=1;}
'''
        p=Path(cls.tmp.name)/'peer.c';p.write_text(preamble+defines+'\n'+source+wrapper)
        lib=p.with_suffix('.so');subprocess.run(['cc','-shared','-fPIC','-Wall','-Werror',str(p),'-o',str(lib)],check=True)
        cls.lib=ctypes.CDLL(str(lib));cls.lib.control.restype=ctypes.c_long;cls.lib.prediction_state.restype=ctypes.c_uint32
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def setUp(self):self.lib.reset()
    def test_park_release_and_reuse(self):
        self.assertEqual(self.lib.control(2,1,0,0),0)
        result=[]
        thread=threading.Thread(target=lambda:result.append(self.lib.control(3,0,0,1)),daemon=True)
        thread.start()
        try:
            end=time.monotonic()+2
            while not self.lib.raw_status(1) and time.monotonic()<end:time.sleep(.001)
            self.assertEqual(self.lib.control(4,1,0,0),1)
            self.assertEqual(self.lib.prediction_state(1),0x80000200)
            self.assertEqual(self.lib.control(5,1,1,0),0)
            thread.join(2);self.assertFalse(thread.is_alive());self.assertEqual(result,[0])
            self.assertEqual(self.lib.control(4,1,0,0),3)
            self.assertEqual(self.lib.prediction_state(1),0x80001230)
            self.assertEqual(self.lib.control(2,1,0,0),0)
        finally:self.lib.force_release(1);thread.join(2)
    def test_cancelled_pending_callback_prevents_reuse_until_exit(self):
        self.assertEqual(self.lib.control(2,1,0,0),0)
        self.assertEqual(self.lib.control(5,1,1,0),0)
        self.assertEqual(self.lib.control(2,1,0,0),-6)
        self.assertEqual(self.lib.control(3,0,0,1),0)
        self.assertEqual(self.lib.prediction_state(1),0x80001230)
        self.assertEqual(self.lib.control(4,1,0,0),3)
        self.assertEqual(self.lib.control(2,1,0,0),0)
    def test_failed_queue_cancellation_allows_reuse(self):
        self.assertEqual(self.lib.control(2,1,0,0),0)
        self.assertEqual(self.lib.control(5,1,0,0),0)
        self.assertEqual(self.lib.control(2,1,0,0),0)
    def test_invalid_peer_and_unprepared_park(self):
        for peer,cpu in ((0,0),(1,1),(2,0)):
            for func in (2,4,5):self.assertEqual(self.lib.control(func,peer,0,cpu),-3)
        self.assertEqual(self.lib.control(3,0,0,1),-3)
        self.assertEqual(self.lib.control(99,1,0,0),-2)
