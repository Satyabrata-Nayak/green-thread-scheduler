/* Correctness under several OS threads.
 *
 * Green threads now genuinely run in parallel, so anything they share needs
 * the same care as real threads. Each thread here owns its own slot and
 * touches nothing else; the shared counter is deliberately atomic, because
 * the point is to check the scheduler delivers every thread exactly once, not
 * to demonstrate a data race.
 *
 * Usage: ./bin/mt_test [workers]   (default 4) */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "gt.h"

#define NTHREADS 64
#define WORK 200000L

static long results[NTHREADS];
static int finished = 0; /* atomic */

static void worker(void *arg) {
    long id = (long)arg;
    long acc = 0;
    for (long i = 0; i < WORK; i++) {
        acc += i % 7;
        if ((i & 0xFFF) == 0) gt_yield(); /* interleave, and invite stealing */
    }
    results[id] = acc;
    __atomic_add_fetch(&finished, 1, __ATOMIC_ACQ_REL);
}

int main(int argc, char **argv) {
    int nw = argc > 1 ? atoi(argv[1]) : 4;

    for (long i = 0; i < NTHREADS; i++) {
        results[i] = -1;
        assert(gt_create(worker, (void *)i) == 0);
    }
    gt_run_on(nw);

    /* Every thread ran exactly once and produced the right answer. */
    long expected = 0;
    for (long i = 0; i < WORK; i++) expected += i % 7;

    assert(finished == NTHREADS && "not every green thread completed");
    for (int i = 0; i < NTHREADS; i++) {
        assert(results[i] == expected && "a green thread produced a wrong result");
    }

    long total_ran = 0, total_stolen = 0, active_workers = 0;
    printf("workers: %d\n", nw);
    for (int i = 0; i < nw; i++) {
        long ran = 0, stolen = 0;
        gt_worker_stats(i, &ran, &stolen);
        printf("  worker %d: ran %5ld slices, %4ld stolen\n", i, ran, stolen);
        total_ran += ran;
        total_stolen += stolen;
        if (ran > 0) active_workers++;
    }
    printf("total: %ld slices, %ld stolen\n", total_ran, total_stolen);

    /* With more than one worker the work must actually be shared -- if only
       one worker ever ran anything, stealing is broken even though the
       results would still be correct. */
    if (nw > 1) {
        assert(active_workers > 1 && "only one worker ever ran a green thread");
        assert(total_stolen > 0 && "no work was ever stolen");
    }

    printf("OK: %d green threads, all correct, spread over %ld workers\n", NTHREADS,
           active_workers);
    return 0;
}
