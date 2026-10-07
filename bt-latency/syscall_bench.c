/* Diagnostic only: timed local costs, not radio throughput acceptance. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>
#include <errno.h>
static uint64_t ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static volatile unsigned char payload[990];
int main(void) {
    const unsigned loops = 2000;
    struct timespec ts;
    int p[2]; unsigned char b=0;
    if (pipe(p)) return 1;
    for (unsigned mode=0;mode<5;mode++) {
        uint64_t start=ns();
        for(unsigned i=0;i<loops;i++) {
            switch(mode) {
            case 0: (void)syscall(SYS_getpid); break;
            case 1: if(clock_gettime(CLOCK_MONOTONIC,&ts)) return 2; break;
            case 2: if(clock_gettime(CLOCK_MONOTONIC_COARSE,&ts)) return 3; break;
            case 3: if(write(p[1],&b,1)!=1 || read(p[0],&b,1)!=1) return 4; break;
            case 4: for(unsigned k=4;k<sizeof(payload);k++) payload[k]=k^i; break;
            }
        }
        uint64_t elapsed=ns()-start;
        printf("mode=%u loops=%u elapsed_ns=%llu per_iteration_ns=%llu\n",
               mode,loops,(unsigned long long)elapsed,(unsigned long long)(elapsed/loops));
    }
    close(p[0]);close(p[1]);
    return 0;
}
