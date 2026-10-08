
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
bool esp32s31_ext_first_use(struct pt_regs *regs)
{
	u16 insn;

	if (!READ_ONCE(s31_ext_lazy_user) || !user_mode(regs) ||
	    current->thread.esp32s31_ext_used)
		return false;
	/* Instructions may start on a two-byte boundary after RVC. */
	if (get_user(insn, (u16 __user *)regs->epc))
		return false;
	/* Match the vendor families in the existing hart0 migration check.
	 * Retry once without advancing PC; ordinary SIGILL is unchanged.
	 */
	if ((insn & 0x7fU) != 0x2bU && (insn & 0x1bU) != 0x1bU)
		return false;
	current->thread.esp32s31_ext_used = true;
	S31_EXT_COUNT(first_use);
	return true;
}static long restore_esp32s31_ext_state(void __user *sc_ext)
{
	long err;

	err = __copy_from_user(&current->thread.esp32s31_ext, sc_ext,
			       sizeof(current->thread.esp32s31_ext));
	if (unlikely(err))
		return err;
	current->thread.esp32s31_ext_active = false;
	/* A handler may use extensions while the interrupted program does not.
	 * Restore live/dirty user state, but leave an empty frame disabled.
	 * Nonzero loop counts are honored even in a user-edited clean frame.
	 */
	current->thread.esp32s31_ext_used =
		(current->thread.esp32s31_ext.hwloop_state & 3) == 3 ||
		(current->thread.esp32s31_ext.pie_state & 3) == 3 ||
		current->thread.esp32s31_ext.hwloop[2] ||
		current->thread.esp32s31_ext.hwloop[5];
	return 0;
}
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
static void s31_radio_queue(void)
{
	/* NULL once the queue is destroyed in shutdown; wake sites stay silent
	 * afterwards (same role as the old worker-pointer checks).  Also
	 * parked permanently after terminal first-pass init failure. */
	if (READ_ONCE(s31_radio_init_dead) || !READ_ONCE(s31_radio_wq))
		return;
	if (READ_ONCE(s31_bt_queue_coalesce) && s31_radio_is_bt_only()) {
		/* The first producer owns queueing; the worker consumes this
		 * token before examining pending event state.
		 */
		if (atomic_xchg(&s31_radio_immediate_pending, 1))
			return;
		hrtimer_try_to_cancel(&s31_radio_hrtimer);
		/* Promote an already delayed pass as well. */
		mod_delayed_work(s31_radio_wq, &s31_radio_work, 0);
	} else {
		hrtimer_try_to_cancel(&s31_radio_hrtimer);
		queue_delayed_work(s31_radio_wq, &s31_radio_work, 0);
	}
}
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
