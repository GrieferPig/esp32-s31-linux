#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sched.h>
#include <assert.h>
extern uint32_t s31_hwloop_setupi(void);
extern void s31_pie_add_u32(const uint32_t*,const uint32_t*,uint32_t*);
static volatile sig_atomic_t seen;
static uint32_t a[4] __attribute__((aligned(16)))={1,2,3,4};
static uint32_t b[4] __attribute__((aligned(16)))={10,20,30,40};
static uint32_t out[4] __attribute__((aligned(16)));
static void handler(int sig) {
 if(sig==SIGUSR2)s31_pie_add_u32(a,b,out);
 seen++;
}
struct counts {unsigned long suspend,restore,first;};
static unsigned long field(char *text,const char *key){
 char *p=strstr(text,key);assert(p);return strtoul(p+strlen(key),NULL,10);
}
static struct counts stats(void) {
 char text[2048];int fd=open("/sys/module/esp32s31_ext/parameters/stats",O_RDONLY);
 assert(fd>=0);ssize_t n=read(fd,text,sizeof(text)-1);assert(n>0);text[n]=0;close(fd);
 return (struct counts){field(text,"bt_syscall_suspend="),field(text,"bt_syscall_restore="),field(text,"first_use=")};
}
static void unused_calls(const char *label) {
 struct counts before=stats();
 for(int i=0;i<100;i++)assert(syscall(SYS_getpid)>0);
 struct counts after=stats();
 if(before.suspend!=after.suspend || before.restore!=after.restore){
  fprintf(stderr,"%s: unexpected SBI transitions %lu/%lu -> %lu/%lu\n",label,before.suspend,before.restore,after.suspend,after.restore);exit(1);
 }
 printf("%s: 100 syscalls, zero extension suspend/restore PASS\n",label);
}
int main(int argc,char **argv) {
 cpu_set_t cpus;CPU_ZERO(&cpus);CPU_SET(argc>1&&!strcmp(argv[1],"--cpu0")?0:1,&cpus);
 assert(!sched_setaffinity(0,sizeof(cpus),&cpus));
 assert(!prctl(PR_SET_NAME,"s31-btstack-sig",0,0,0));
 unused_calls(argc>1?"exec reset":"fresh exec");
 if(argc>1&&!strcmp(argv[1],"--cpu0")){
  struct counts before=stats();assert(s31_hwloop_setupi()==5);assert(sched_getcpu()==0);
  struct counts after=stats();assert(after.first==before.first+1);
  puts("CPU0 HWLoop disabled before first use, first-use activation PASS");return 0;
 }
 if(argc>1)return 0;
 struct sigaction sa={.sa_handler=handler};sigemptyset(&sa.sa_mask);
 assert(!sigaction(SIGUSR1,&sa,NULL));assert(!sigaction(SIGUSR2,&sa,NULL));
 assert(!kill(getpid(),SIGUSR1));assert(seen==1);
 unused_calls("plain signal return");
 struct counts before=stats();
 assert(!kill(getpid(),SIGUSR2));assert(seen==2);
 for(int i=0;i<4;i++)assert(out[i]==a[i]+b[i]);
 struct counts after=stats();assert(after.first==before.first+1);
 unused_calls("PIE handler return to unused program");
 s31_pie_add_u32(a,b,out);
 before=stats();
 for(int i=0;i<100;i++)assert(syscall(SYS_getpid)>0);
 after=stats();assert(after.suspend>=before.suspend+100 && after.restore>=before.restore+100);
 puts("program first use: active context save/restore PASS");
 pid_t child=fork();assert(child>=0);
 if(!child){execl(argv[0],argv[0],"--unused",NULL);perror("exec");_exit(1);}
 int status;assert(waitpid(child,&status,0)==child);assert(WIFEXITED(status)&&!WEXITSTATUS(status));
 puts("unused/signal/first-use/exec policy PASS");return 0;
}
