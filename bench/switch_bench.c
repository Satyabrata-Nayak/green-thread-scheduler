/* Context-switch latency microbenchmark.
 *
 * Two green threads ping-pong through gt_yield(). Each gt_yield() costs two
 * stack switches -- thread -> scheduler, then scheduler -> other thread --
 * plus one pass of the scheduler's run-queue scan, so the per-switch figure
 * below is the honest "what does one switch cost in this scheduler" number
 * rather than a bare swapcontext microbenchmark.
 *
 * Build produces two binaries from the same source: bin/switch_bench uses the
 * hand-written x86-64 switch, bin/switch_bench_uctx uses swapcontext(). */

#include <stdio.h>
#include <time.h>

#include "gt.h"

#ifdef GT_UCONTEXT_SWITCH
#define BACKEND "ucontext (swapcontext)"
#else
#define BACKEND "hand-written x86-64"
#endif

#define ROUNDS 1000000L
#define WARMUP 20000L

static long rounds = 0;
static long yields = 0;

static void pinger(void *arg) {
    (void)arg;
    for (long i = 0; i < rounds; i++) {
        yields++;
        gt_yield();
    }
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* Runs a full ping-pong to completion and returns nanoseconds elapsed. */
static double run_pass(long n) {
    rounds = n;
    yields = 0;
    gt_create(pinger, NULL);
    gt_create(pinger, NULL);

    double start = now_ns();
    gt_run();
    return now_ns() - start;
}

int main(void) {
    /* Warm-up pass, discarded: pays the page faults on both 64KB stacks and
       the first-touch cost of the thread table before we start the clock. */
    run_pass(WARMUP);

    double elapsed = run_pass(ROUNDS);
    long total_yields = yields;
    long switches = total_yields * 2; /* thread -> sched -> thread */

    printf("backend:        %s\n", BACKEND);
    printf("gt_yield calls: %ld\n", total_yields);
    printf("stack switches: %ld\n", switches);
    printf("elapsed:        %.3f ms\n", elapsed / 1e6);
    printf("per gt_yield:   %.1f ns\n", elapsed / (double)total_yields);
    printf("per switch:     %.1f ns\n", elapsed / (double)switches);
    return 0;
}
