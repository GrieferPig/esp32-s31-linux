#ifndef S31_COREMARK_MONOTONIC_H
#define S31_COREMARK_MONOTONIC_H

/*
 * CoreMark's POSIX port measures an elapsed interval with CLOCK_REALTIME.
 * Include time.h first, then redirect that use to a clock which cannot jump
 * when the system wall clock is corrected after boot.
 */
#include <time.h>
#undef CLOCK_REALTIME
#define CLOCK_REALTIME CLOCK_MONOTONIC

#endif
