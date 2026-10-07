
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32;
#define ACTORS 3
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
struct task_struct {u32 s31_cost_actor,s31_cost_category,s31_cost_base; bool idle;};
#define is_idle_task(t) ((t)->idle)
enum s31_cost_cat {
 S31_COST_OUTSIDE, S31_COST_PAYLOAD, S31_COST_INTEGRATION,
 S31_COST_GATE, S31_COST_WAIT, S31_COST_WAKE, S31_COST_SYNC,
 S31_COST_QUEUE, S31_COST_SCHED, S31_COST_READ, S31_COST_WRITE,
 S31_COST_SELECT, S31_COST_TIME, S31_COST_SYSCALL_OTHER,
 S31_COST_IRQ, S31_COST_SOFTIRQ, S31_COST_IDLE, S31_COST_CATS
};
enum s31_cost_event {
 S31_EV_SWITCH, S31_EV_WORK, S31_EV_PASS, S31_EV_QUEUE,
 S31_EV_GATE_ENTER, S31_EV_GATE_LEAVE, S31_EV_GATE_SUSPEND,
 S31_EV_GATE_RESUME, S31_EV_SYNC_WAIT, S31_EV_SYNC_WAKE,
 S31_EV_DEFERRED_ISR, S31_EV_DIRECT_ISR, S31_EV_IRQ,
 S31_EV_SOFTIRQ, S31_EV_EVENTS
};
struct s31_cost_cpu {
 u64 ns[ACTORS][S31_COST_CATS];
 u32 entries[ACTORS][S31_COST_CATS];
 u32 events[S31_EV_EVENTS];
 u64 start, end, last;
 u32 actor, cat, irq_depth, soft_depth, irq_cat, soft_cat;
 u32 irq_stack[8];
 u32 clock_errors, state_errors;
 bool active;
};
static void cost_state(struct s31_cost_cpu *p, struct task_struct *task)
{
 p->actor = min_t(u32, task->s31_cost_actor, ACTORS - 1);
 if (p->irq_depth)
  p->cat = p->irq_cat;
 else if (p->soft_depth)
  p->cat = p->soft_cat;
 else if (task->s31_cost_category)
  p->cat = task->s31_cost_category;
 else if (is_idle_task(task))
  p->cat = S31_COST_IDLE;
 else
  p->cat = task->s31_cost_base;
 if (p->cat >= S31_COST_CATS) {
  p->cat = S31_COST_OUTSIDE;
  p->state_errors++;
 }
}static void cost_account(struct s31_cost_cpu *p, u64 now)
{
 if (!p->active) return;
 if (now < p->last) p->clock_errors++;
 else p->ns[p->actor][p->cat] += now - p->last;
 p->last = now;
}
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
