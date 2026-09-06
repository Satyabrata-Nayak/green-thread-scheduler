#ifndef GT_H
#define GT_H

#include <stddef.h>

#define GT_MAX_THREADS 128
#define GT_STACK_SIZE (64 * 1024)
#define GT_TIMESLICE_US 10000 /* preemption interval, in CPU-time microseconds */

/* Create a new green thread running fn(arg). Returns 0 on success. */
int gt_create(void (*fn)(void *), void *arg);

/* Voluntarily give up the CPU to the next READY thread. */
void gt_yield(void);

/* Run the scheduler loop until all threads have finished. */
void gt_run(void);

#endif
