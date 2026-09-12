// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct reader { const unsigned char *map; size_t size; int stopfd; unsigned long passes; int error; };

static int pin(unsigned int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set); CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

static void *read_pages(void *arg)
{
    struct reader *r = arg;
    char stop;
    if (pin(1)) { r->error = errno; return NULL; }
    for (;;) {
        ssize_t n = read(r->stopfd, &stop, 1);
        if (n == 1) break;
        if (n < 0 && errno != EAGAIN && errno != EINTR) { r->error = errno; break; }
        for (size_t i = 0; i < r->size; i += 4096)
            if (*(const volatile unsigned char *)(r->map + i) != 0x5a) { r->error = EILSEQ; return NULL; }
        r->passes++;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    unsigned long count = 2000, done = 0;
    char *end;
    const size_t size = 32 * 4096;
    int fd[2], rc = 1, err;
    pthread_t thread;
    struct reader r = { .size = size };
    if (argc > 2) { fprintf(stderr, "usage: %s [1..100000 permission changes]\n", argv[0]); return 2; }
    if (argc == 2) {
        errno = 0; count = strtoul(argv[1], &end, 10);
        if (errno || !*argv[1] || *end || !count || count > 100000) return 2;
    }
    alarm(90);
    if (pin(0) || pipe2(fd, O_NONBLOCK | O_CLOEXEC)) { perror("setup"); return 1; }
    r.map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r.map == MAP_FAILED) { perror("mmap"); goto close_pipe; }
    memset((void *)r.map, 0x5a, size);
    r.stopfd = fd[0];
    err = pthread_create(&thread, NULL, read_pages, &r);
    if (err) { fprintf(stderr, "pthread_create: %s\n", strerror(err)); goto unmap; }
    usleep(20000);
    for (; done < count; done++) {
        int prot = PROT_READ | ((done & 1) ? PROT_WRITE : 0);
        if (mprotect((void *)r.map, size, prot)) { perror("mprotect"); break; }
    }
    if (write(fd[1], "x", 1) != 1) { perror("stop"); goto join; }
    rc = 0;
join:
    err = pthread_join(thread, NULL);
    if (err || r.error || !r.passes || done != count) rc = 1;
    printf("%s tlb-stress changes=%lu reader_passes=%lu reader_error=%d join_error=%d cpus=0,1 bytes=%zu\n",
           rc ? "FAIL" : "PASS", done, r.passes, r.error, err, size);
unmap:
    munmap((void *)r.map, size);
close_pipe:
    close(fd[0]); close(fd[1]);
    return rc;
}
