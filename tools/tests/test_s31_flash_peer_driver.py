"""Compile the real Linux Flash handoff against failure-injectable SBI hooks."""
import ctypes
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
class FlashDriverPeerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory()
        text=(ROOT/'linux-esp32-s31/drivers/mtd/devices/esp32s31_flash.c').read_text()
        source=text[text.index('/* Use the same IRQ'):text.index('static int esp32s31_flash_read')]
        defines='\n'.join(l for l in text.splitlines() if l.startswith('#define ESP32S31_SBI_'))
        preamble=r'''#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint32_t u32;
struct sbiret {long error,value;};
static int scenario,off,queue_calls,end_calls,prep_calls,polls,status_calls,queued_arg,bad_pointer,locks,pinned;
static unsigned int nr_cpu_ids=2;
static void cpus_read_lock(void){++locks;}
static void cpus_read_unlock(void){--locks;}
static int get_cpu(void){++pinned;return 0;}
static void put_cpu(void){--pinned;}
static bool cpu_online(int cpu){return scenario!=7;}
static bool irqs_disabled(void){return off;}
void esp32s31_irq_poll(void){++polls;}
static void cpu_relax(void){}
static uintptr_t virt_to_phys(void *p){return (uintptr_t)p;}
static int smp_call_function_single(int cpu,void (*fn)(void*),void *data,bool wait){
 ++queue_calls;if(data)bad_pointer=1;return scenario==2?-6:0;
}
static struct sbiret sbi_ecall(unsigned long ext,unsigned long func,unsigned long a0,unsigned long a1,unsigned long a2,unsigned long a3,unsigned long a4,unsigned long a5){
 struct sbiret ret={0,0};
 switch(func){
 case 2:++prep_calls;if(scenario==1)ret.error=-2;break;
 case 4:if(scenario==3){ret.error=-1;ret.value=21;}else ret.value=status_calls++?1:0;break;
 case 0:if(scenario==4)ret.value=7;break;
 case 5:++end_calls;queued_arg=a1;if(scenario==5){ret.error=-1;ret.value=9;}break;
 }
 return ret;
}
'''
        wrapper=r'''static struct sbiret last;
long invoke(int failure,int irq_off){
 scenario=failure;off=irq_off;queue_calls=end_calls=prep_calls=polls=status_calls=bad_pointer=locks=pinned=0;queued_arg=-1;
 last=esp32s31_flash_ecall(0,0xb30000,NULL,32);return last.error;
}
long result_value(void){return last.value;}
int end_count(void){return end_calls;}
int prep_count(void){return prep_calls;}
int queue_count(void){return queue_calls;}
int queued(void){return queued_arg;}
int poll_count(void){return polls;}
int balanced(void){return !locks&&!pinned&&!bad_pointer;}
'''
        p=Path(cls.tmp.name)/'driver.c';p.write_text(preamble+defines+'\n'+source+wrapper)
        lib=p.with_suffix('.so');subprocess.run(['cc','-shared','-fPIC','-Wall','-Werror',str(p),'-o',str(lib)],check=True)
        cls.lib=ctypes.CDLL(str(lib));cls.lib.invoke.restype=ctypes.c_long;cls.lib.result_value.restype=ctypes.c_long
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def check_balanced(self):self.assertEqual(self.lib.balanced(),1)
    def test_old_firmware_and_offline_peer_fail_before_queue(self):
        for scenario,expected in ((1,-2),(7,-1)):
            self.assertEqual(self.lib.invoke(scenario,0),expected)
            self.assertEqual(self.lib.queue_count(),0);self.assertEqual(self.lib.end_count(),0);self.check_balanced()
    def test_queue_failure_releases_without_late_callback(self):
        self.assertEqual(self.lib.invoke(2,0),-1)
        self.assertEqual(self.lib.end_count(),1);self.assertEqual(self.lib.queued(),0);self.check_balanced()
    def test_status_and_rom_failure_release_queued_peer_and_preserve_error(self):
        for scenario,error,value in ((3,-1,21),(4,0,7)):
            self.assertEqual(self.lib.invoke(scenario,0),error)
            self.assertEqual(self.lib.result_value(),value)
            self.assertEqual(self.lib.end_count(),1);self.assertEqual(self.lib.queued(),1);self.check_balanced()
    def test_release_failure_and_irq_disabled_poll_guard(self):
        self.assertEqual(self.lib.invoke(5,0),-1);self.assertEqual(self.lib.result_value(),9);self.check_balanced()
        for off in (0,1):
            self.assertEqual(self.lib.invoke(0,off),0)
            self.assertEqual(self.lib.poll_count(),0 if off else 1)
            self.assertEqual(self.lib.queued(),1);self.check_balanced()
