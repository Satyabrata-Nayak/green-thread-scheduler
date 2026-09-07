/* Work-stealing scaling benchmark.
 *
 * A fixed pile of CPU-bound green threads is run repeatedly, with the number
 * of backing OS threads varied from 1 up to the core count. Green threads
 * cannot make a single core faster -- what the M:N scheduler buys is the
 * ability to spread them over real cores, so the number that matters here is
 * how close the speedup tracks the worker count.
 *
 * Usage: ./bin/scaling_bench [max_workers] */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "gt.h"

#define NTHREADS 64
#define WORK 20000000L

static volatile long sink[NTHREADS];

static void spinner(void *arg) {
    long id = (long)arg;
    long acc = 0;
    for (long i = 0; i < WORK; i++) {
        acc += i % 7;
        /* Yield occasionally: a thread that never yields cannot migrate, and
           without migration there is nothing for a thief to take. */
        if ((i & 0xFFFF) == 0) gt_yield();
    }
    sink[id] = acc;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double run_pass(int nw, long *stolen_out) {
    for (long i = 0; i < NTHREADS; i++) gt_create(spinner, (void *)i);

    double start = now_s();
    gt_run_on(nw);
    double elapsed = now_s() - start;

    long stolen = 0;
    for (int i = 0; i < nw; i++) {
        long r = 0, s = 0;
        gt_worker_stats(i, &r, &s);
        stolen += s;
    }
    *stolen_out = stolen;
    return elapsed;
}

int main(int argc, char **argv) {
    int cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    int max_workers = argc > 1 ? atoi(argv[1]) : cores;
    if (max_workers > GT_MAX_WORKERS) max_workers = GT_MAX_WORKERS;
    if (max_workers < 1) max_workers = 1;

    printf("%d green threads, %ld iterations each, %d cores online\n\n", NTHREADS, WORK,
           cores);

    long dummy;
    run_pass(1, &dummy); /* warm-up, discarded */

    double baseline = 0;
    printf("workers   wall time   speedup   efficiency   steals\n");
    printf("-------   ---------   -------   ----------   ------\n");
    for (int nw = 1; nw <= max_workers; nw++) {
        long stolen = 0;
        double t = run_pass(nw, &stolen);
        if (nw == 1) baseline = t;
        double speedup = baseline / t;
        printf("%7d   %7.3f s   %6.2fx   %8.0f%%   %6ld\n", nw, t, speedup,
               100.0 * speedup / nw, stolen);
        fflush(stdout);
    }

    return 0;
}
