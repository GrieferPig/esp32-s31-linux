/* Steady wall-clock/NO_HZ idle-residency sample; no output in-window. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
static const char *diag_path="/sys/module/esp32s31_ext/parameters/diagnostics";
static const char *stats_path="/sys/module/esp32s31_ext/parameters/stats";
static void diag(int fd, char v) {
 if(lseek(fd,0,SEEK_SET)<0 || write(fd,&v,1)!=1){perror("diagnostics");exit(1);}
}
static void extstats(int fd,char *out,size_t len) {
 if(lseek(fd,0,SEEK_SET)<0){perror("stats seek");exit(1);}
 ssize_t n=read(fd,out,len-1);if(n<=0){perror("stats read");exit(1);}out[n]=0;
}
struct sample { struct timespec begin,end; char stat[8192]; ssize_t length; };
static uint64_t ns(struct timespec t){ return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec; }
static int snapshot(int fd,struct sample *s){
 if(lseek(fd,0,SEEK_SET)<0 || clock_gettime(CLOCK_MONOTONIC,&s->begin))return -1;
 s->length=read(fd,s->stat,sizeof(s->stat)-1);
 if(clock_gettime(CLOCK_MONOTONIC,&s->end) || s->length<=0)return -1;
 s->stat[s->length]=0;return 0;
}
static void dump(const char *phase,struct sample *s){
 printf("CPU_SNAPSHOT phase=%s begin_ns=%llu end_ns=%llu\n",phase,
        (unsigned long long)ns(s->begin),(unsigned long long)ns(s->end));
 char *line=s->stat;
 while(!strncmp(line,"cpu",3)){
  char *end=strchr(line,'\n');if(!end)break;
  fwrite(line,1,end-line+1,stdout);line=end+1;
 }
}
int main(int argc,char **argv){
 int seconds=argc>1?atoi(argv[1]):30;if(seconds<1||seconds>120)return 2;
 int fd=open("/proc/stat",O_RDONLY);if(fd<0){perror("open");return 1;}
 int extdiag=open(diag_path,O_WRONLY),extfd=open(stats_path,O_RDONLY);
 if(extdiag<0||extfd<0){perror("extension knobs");return 1;}
 char ext_before[2048],ext_after[2048];struct timespec ext_start,ext_end;
 diag(extdiag,'0');extstats(extfd,ext_before,sizeof(ext_before));
 clock_gettime(CLOCK_MONOTONIC,&ext_start);diag(extdiag,'1');
 struct sample a,b;if(snapshot(fd,&a)){perror("snapshot");return 1;}
 struct timespec deadline=a.end;deadline.tv_sec+=seconds;
 int rc;do{rc=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&deadline,NULL);}while(rc==EINTR);
 if(rc||snapshot(fd,&b)){fprintf(stderr,"sample failed %d\n",rc);return 1;}
 diag(extdiag,'0');clock_gettime(CLOCK_MONOTONIC,&ext_end);
 extstats(extfd,ext_after,sizeof(ext_after));
 close(extdiag);close(extfd);close(fd);dump("before",&a);dump("after",&b);
 printf("EXT_WINDOW begin_ns=%llu end_ns=%llu\n",(unsigned long long)ns(ext_start),(unsigned long long)ns(ext_end));
 printf("EXT_BEFORE %sEXT_AFTER %s",ext_before,ext_after);return 0;
}
