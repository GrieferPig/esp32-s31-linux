from pathlib import Path
import subprocess,re,gzip
d=Path(__file__).resolve().parent
def source(name):
 p=d/name
 return p.read_text() if p.exists() else gzip.decompress(Path(str(p)+'.gz').read_bytes()).decode()
src=source('diagnostic/kernel/s31_cost.c')
def function(name):
 start=src.index('static void '+name+'(');brace=src.index('{',start);depth=1;i=brace+1
 while depth:
  depth+=(src[i]=='{')-(src[i]=='}');i+=1
 return src[start:i]
struct=src[src.index('struct s31_cost_cpu {'):src.index('static DEFINE_PER_CPU')]
header=source('diagnostic/include/linux/s31_cost.h')
enum=header[header.index('enum s31_cost_cat'):header.index('u32 s31_cost_enter')]
pre=r"""
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32;
#define ACTORS 3
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
struct task_struct {u32 s31_cost_actor,s31_cost_category,s31_cost_base; bool idle;};
#define is_idle_task(t) ((t)->idle)
"""
test=r"""
static void step(struct s31_cost_cpu *p,struct task_struct *task,u64 time)
{cost_account(p,time);cost_state(p,task);}
int main(void)
{
 struct s31_cost_cpu p={.active=true};
 struct task_struct radio={.s31_cost_actor=1,.s31_cost_base=S31_COST_PAYLOAD};
 struct task_struct peer={.s31_cost_actor=2};
 cost_state(&p,&radio);
 /* Five units payload, five gate; then radio sleeps while peer runs. */
 cost_account(&p,5);radio.s31_cost_category=S31_COST_GATE;cost_state(&p,&radio);
 step(&p,&peer,10);step(&p,&radio,40);
 cost_account(&p,45);radio.s31_cost_category=0;cost_state(&p,&radio);
 assert(p.ns[1][S31_COST_PAYLOAD]==5);
 assert(p.ns[1][S31_COST_GATE]==10); /* Sleeping 30 excluded. */
 assert(p.ns[2][S31_COST_OUTSIDE]==30);
 /* IRQ time exclusive, with a nested payload callback and bridge override. */
 cost_account(&p,50);p.irq_depth=1;p.irq_cat=S31_COST_IRQ;cost_state(&p,&radio);
 cost_account(&p,52);p.irq_cat=S31_COST_PAYLOAD;cost_state(&p,&radio);
 cost_account(&p,55);p.irq_cat=S31_COST_WAKE;cost_state(&p,&radio);
 cost_account(&p,56);p.irq_cat=S31_COST_PAYLOAD;cost_state(&p,&radio);
 cost_account(&p,57);p.irq_cat=S31_COST_IRQ;cost_state(&p,&radio);
 cost_account(&p,58);p.irq_depth=0;cost_state(&p,&radio);
 assert(p.ns[1][S31_COST_IRQ]==3);
 assert(p.ns[1][S31_COST_WAKE]==1);
 assert(p.ns[1][S31_COST_PAYLOAD]==14);
 /* Stop freezes counters, capacity conserved exactly. */
 u64 total=0;for(int a=0;a<ACTORS;a++)for(int c=0;c<S31_COST_CATS;c++)total+=p.ns[a][c];
 assert(total==58);p.active=false;cost_account(&p,1000);
 u64 after=0;for(int a=0;a<ACTORS;a++)for(int c=0;c<S31_COST_CATS;c++)after+=p.ns[a][c];
 assert(after==total);
 /* Invalid state and reversed clock reported instead of unsigned wrap. */
 p.active=true;cost_account(&p,57);assert(p.clock_errors==1);
 radio.s31_cost_category=S31_COST_CATS;cost_state(&p,&radio);assert(p.state_errors==1);
 return 0;
}
"""
out=d/'accounting-test.c';out.write_text(pre+enum+struct+function('cost_state')+function('cost_account')+test)
argv=['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-o',str(d/'accounting-test'),str(out)]
subprocess.run(argv,check=True);subprocess.run([str(d/'accounting-test')],check=True)
print('PASS: sleep excluded, nested callback/IRQ exclusive, capacity conserved, freeze stable, invalid clock/state detected')
