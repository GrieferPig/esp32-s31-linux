from pathlib import Path
import subprocess, hashlib,json,shutil
d=Path(__file__).resolve().parent;r=d.parents[2];k=r/'linux-esp32-s31'
def fn(file,signature):
 t=(k/file).read_text();a=t.index(signature);b=t.index('{',a);level=1;c=b+1
 while level:
  level+= (t[c]=='{')-(t[c]=='}');c+=1
 return t[a:c]
prefix=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
struct pt_regs { unsigned long epc; bool user; };
struct ext_state {u32 hwloop[6], hwloop_state, pie_state, pie[54];};
struct task_struct { struct { bool esp32s31_ext_used, esp32s31_ext_active; struct ext_state esp32s31_ext; } thread; };
static struct task_struct task, *current=&task;
static bool s31_ext_lazy_user, fault;
static unsigned first_use;
#define READ_ONCE(x) (x)
#define user_mode(r) ((r)->user)
#define __user
#define get_user(out,p) (fault?1:((out)=*(p),0))
#define S31_EXT_COUNT(member) (++first_use)
#define unlikely(x) (x)
#define __copy_from_user(dst,src,n) (fault?1:(memcpy(dst,src,n),0))
"""
queue=r"""
typedef struct {int value;} atomic_t;
static atomic_t s31_radio_immediate_pending;
static bool s31_bt_queue_coalesce, bt_only, s31_radio_init_dead;
static void *s31_radio_wq=(void*)1;
static int s31_radio_hrtimer, s31_radio_work, cancel_count, mod_count, queue_count;
static int atomic_xchg(atomic_t *p,int v){int old=p->value;p->value=v;return old;}
static bool s31_radio_is_bt_only(void){return bt_only;}
static void hrtimer_try_to_cancel(void*p){cancel_count++;}
static void mod_delayed_work(void*w,void*work,int delay){assert(!delay);mod_count++;}
static void queue_delayed_work(void*w,void*work,int delay){assert(!delay);queue_count++;}
"""
main=r"""
int main(void) {
 u32 insn=0x13;struct pt_regs regs={(unsigned long)&insn,true};
 s31_ext_lazy_user=true;
 assert(!esp32s31_ext_first_use(&regs));assert(!first_use);
 insn=0x2b;fault=true;assert(!esp32s31_ext_first_use(&regs));fault=false;
 regs.user=false;assert(!esp32s31_ext_first_use(&regs));regs.user=true;
 assert(esp32s31_ext_first_use(&regs));assert(first_use==1);
 assert(regs.epc==(unsigned long)&insn);assert(task.thread.esp32s31_ext_used);
 assert(!esp32s31_ext_first_use(&regs)); /* unsupported opcode retries at most once */
 task.thread.esp32s31_ext_used=false;insn=0x1b;
 assert(esp32s31_ext_first_use(&regs));assert(first_use==2);
 task.thread.esp32s31_ext_used=false;s31_ext_lazy_user=false;
 assert(!esp32s31_ext_first_use(&regs));
 struct ext_state frame={0};
 task.thread.esp32s31_ext_used=true;task.thread.esp32s31_ext_active=true;
 assert(!restore_esp32s31_ext_state(&frame));
 assert(!task.thread.esp32s31_ext_used && !task.thread.esp32s31_ext_active);
 frame.pie_state=3;assert(!restore_esp32s31_ext_state(&frame));assert(task.thread.esp32s31_ext_used);
 frame.pie_state=0;frame.hwloop[2]=1;assert(!restore_esp32s31_ext_state(&frame));assert(task.thread.esp32s31_ext_used);
 fault=true;assert(restore_esp32s31_ext_state(&frame));fault=false;
 bt_only=true;s31_bt_queue_coalesce=true;
 s31_radio_queue();assert(mod_count==1&&cancel_count==1);
 for(int i=0;i<100;i++)s31_radio_queue();
 assert(mod_count==1); /* coalesced producers */
 atomic_xchg(&s31_radio_immediate_pending,0); /* worker begins consumption */
 s31_radio_queue();assert(mod_count==2); /* event during running worker */
 bt_only=false;s31_radio_queue();assert(queue_count==1);
 bt_only=true;s31_bt_queue_coalesce=false;s31_radio_queue();assert(queue_count==2);
 s31_radio_init_dead=true;s31_radio_queue();assert(queue_count==2);
 s31_radio_init_dead=false;s31_radio_wq=0;s31_radio_queue();assert(queue_count==2);
 return 0;
}
"""
c=prefix+fn('arch/riscv/kernel/esp32s31-ext.c','bool esp32s31_ext_first_use(')+fn('arch/riscv/kernel/signal.c','static long restore_esp32s31_ext_state(')+queue+fn('drivers/platform/esp32s31-radio-smode.c','static void s31_radio_queue(void)\n{')+main
(d/'candidate-test.c').write_text(c)
subprocess.run(['cc','-std=c11','-Wall','-Wno-unused-parameter','-Werror',str(d/'candidate-test.c'),'-o',str(d/'candidate-test')],check=True)
subprocess.run([str(d/'candidate-test')],check=True)
scratch=d/'scratch-v2-alignment';scratch.mkdir(exist_ok=False)
hashes=json.loads((d/'original-hashes.json').read_text())
for f,h in hashes.items():
 p=d/'original'/f;assert hashlib.sha256(p.read_bytes()).hexdigest()==h
 out=scratch/f;out.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,out)
x=subprocess.run(['patch','-p1','--batch','--forward','-i',str(d/'candidate.patch')],cwd=scratch,capture_output=True)
(d/'scratch-apply.log').write_bytes(x.stdout+x.stderr);assert x.returncode==0
matches={f:(scratch/f).read_bytes()==(k/f).read_bytes() for f in hashes}
assert all(matches.values());(d/'scratch-byte-match.json').write_text(json.dumps(matches,indent=2))
subprocess.run(['git','diff','--check'],cwd=k,check=True)
print('Actual first-use and queue functions passed control-flow tests; all nine scratch-applied files match.')
