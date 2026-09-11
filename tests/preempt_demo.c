/* Preemption demo: none of these threads ever calls gt_yield(). They spin in
   a tight compute loop, so without timer preemption thread 0 would run to
   completion before thread 1 ever starts.

   Nothing is printed from inside a green thread on purpose: a thread
   preempted mid-printf while another calls printf can corrupt glibc's stdio
   buffer (its stream lock is recursive per OS thread, and all green threads
   share one OS thread). Threads record their scheduling into a shared trace
   array instead, and main prints it after gt_run returns. */

#include <assert.h>
#include <stdio.h>

#include "gt.h"

#define NTHREADS 4
#define WORK_ITERS 30000000L
#define TRACE_MAX 512

/* volatile is load-bearing, not decoration: the spin loop below contains no
   function calls, so at -O2 the compiler would happily cache last_run in a
   register for the whole loop and the thread would never notice it had been
   descheduled and resumed. Same reason the scheduler's in_switch flag is
   volatile -- these are written by a control flow the optimiser cannot see. */
static int trace[TRACE_MAX];
static volatile int trace_len = 0;
static volatile int last_run = -1;
static volatile long sink[NTHREADS];
static volatile long switches[NTHREADS];

static void spinner(void *arg) {
    long id = (long)arg;
    for (long i = 0; i < WORK_ITERS; i++) {
        /* Detect that we were away and came back: the only writer at any
           instant is the one running green thread, so no lock is needed. */
        if (last_run != (int)id) {
            last_run = (int)id;
            switches[id]++;
            if (trace_len < TRACE_MAX) trace[trace_len++] = (int)id;
        }
        sink[id] += i;
    }
}

int main(void) {
    for (long i = 0; i < NTHREADS; i++) gt_create(spinner, (void *)i);
    gt_run();

    printf("schedule trace (thread id each time the CPU changed hands):\n");
    for (int i = 0; i < trace_len; i++) {
        printf("%d%s", trace[i], (i + 1) % 32 ? " " : "\n");
    }
    printf("\n\ntimes scheduled per thread:\n");
    for (int i = 0; i < NTHREADS; i++) {
        printf("  thread %d: %ld slices\n", i, switches[i]);
    }

    /* Without preemption each thread runs start to finish, so the trace is
       exactly NTHREADS entries long and every thread is scheduled once. */
    assert(trace_len > NTHREADS && "threads were never preempted");
    for (int i = 0; i < NTHREADS; i++) {
        assert(switches[i] > 1 && "a thread ran to completion without preemption");
    }
    printf("\nOK: every thread was preempted and resumed multiple times\n");
    return 0;
}
