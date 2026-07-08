#include <stdio.h>

int main(void)
{
    volatile int *null_ptr = (volatile int *)0;

    puts("segfault: writing through a NULL pointer");
    fflush(stdout);

    *null_ptr = 1;
    return 0;
}
