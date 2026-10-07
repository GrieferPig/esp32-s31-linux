/* Host correctness check: recursive and cross-thread callback wakeups. */
#define _POSIX_C_SOURCE 200809
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
static unsigned long wake_writes;
static ssize_t counted_write(int fd, const void *data, size_t size){
    __atomic_fetch_add(&wake_writes,1,__ATOMIC_RELAXED);
    return write(fd,data,size);
}
#define write counted_write
#include S31_RUNLOOP_SOURCE
#undef write

static btstack_context_callback_registration_t chain_reg, cross_reg;
static unsigned chain_count, cross_count, cross_target;
static sem_t acknowledged;
static void maybe_done(void){
    if (chain_count==512 && cross_count==cross_target) btstack_run_loop_posix_trigger_exit();
}
static void chain_callback(void *context){
    (void)context;
    chain_count++;
    if (chain_count<512) btstack_run_loop_execute_on_main_thread(&chain_reg);
    maybe_done();
}
static void cross_callback(void *context){
    (void)context;
    cross_count++;
    sem_post(&acknowledged);
    maybe_done();
}
static void *producer(void *context){
    (void)context;
    for (unsigned i=0;i<cross_target;i++){
        btstack_run_loop_execute_on_main_thread(&cross_reg);
        while (sem_wait(&acknowledged) && errno==EINTR) {}
    }
    return NULL;
}
int main(int argc,char **argv){
    pthread_t thread;
    cross_target=(argc>1 && argv[1][0]=='m') ? 2000 : 0;
    sem_init(&acknowledged,0,0);
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());
    chain_reg.callback=chain_callback;cross_reg.callback=cross_callback;
    btstack_run_loop_execute_on_main_thread(&chain_reg);
    if (cross_target) pthread_create(&thread,NULL,producer,NULL);
    btstack_run_loop_execute();
    if (cross_target) pthread_join(thread,NULL);
    printf("chain=%u cross=%u wake_writes=%lu\n",chain_count,cross_count,wake_writes);
    return !(chain_count==512 && cross_count==cross_target);
}
