#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    pid_t pid;

    errno = 0;
    pid = fork();

    if (pid < 0) {
        printf("fork() failed\n");
        printf("errno = %d (%s)\n", errno, strerror(errno));

        if (errno == ENOSYS)
            printf("Likely NOMMU Linux\n");
        else
            printf("Cannot determine MMU status from this failure alone\n");

        return 1;
    }

    if (pid == 0) {
        _exit(0);
    }

    waitpid(pid, NULL, 0);

    printf("fork() succeeded\n");
    printf("Kernel supports separate process address spaces\n");
    printf("MMU Linux is strongly indicated\n");

    return 0;
}