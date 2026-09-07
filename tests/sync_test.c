/* Mutual exclusion and channel semantics, exercised across several OS
   threads so the primitives face genuine parallelism rather than just
   interleaving.

   Usage: ./bin/sync_test [workers]   (default 4) */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "gt.h"

/* Heavy contention on purpose -- 16 threads fighting over one lock, each
   holding it across a yield. Kept modest because every contended acquisition
   costs a full scheduler turn, which is exactly the trade being tested. */
#define LOCKERS 16
#define BUMPS 3000

#define PRODUCERS 4
#define CONSUMERS 4
#define PER_PRODUCER 500

/* Deliberately NOT atomic: a plain counter is the only honest test of whether
   the mutex actually excludes. If it does not, this ends up short. */
static long counter = 0;
static gt_mutex counter_lock = GT_MUTEX_INIT;

static void locker(void *arg) {
    (void)arg;
    for (int i = 0; i < BUMPS; i++) {
        gt_mutex_lock(&counter_lock);
        long v = counter;
        gt_yield(); /* widen the critical section: a broken lock loses here */
        counter = v + 1;
        gt_mutex_unlock(&counter_lock);
    }
}

static gt_chan chan;
static long produced = 0; /* atomic */
static long consumed = 0; /* atomic */
static long sum_received = 0;

static void producer(void *arg) {
    long id = (long)arg;
    for (long i = 0; i < PER_PRODUCER; i++) {
        long value = id * PER_PRODUCER + i;
        assert(gt_chan_send(&chan, (void *)(value + 1)) == 0);
        __atomic_add_fetch(&produced, 1, __ATOMIC_ACQ_REL);
    }
}

static void consumer(void *arg) {
    (void)arg;
    void *v;
    while (gt_chan_recv(&chan, &v) == 0) {
        __atomic_add_fetch(&sum_received, (long)v, __ATOMIC_ACQ_REL);
        __atomic_add_fetch(&consumed, 1, __ATOMIC_ACQ_REL);
    }
}

static void closer(void *arg) {
    (void)arg;
    /* Wait until every producer is done, then close so consumers can finish. */
    while (__atomic_load_n(&produced, __ATOMIC_ACQUIRE) < PRODUCERS * PER_PRODUCER) {
        gt_yield();
    }
    gt_chan_close(&chan);
}

int main(int argc, char **argv) {
    int nw = argc > 1 ? atoi(argv[1]) : 4;

    /* --- mutual exclusion --- */
    for (int i = 0; i < LOCKERS; i++) assert(gt_create(locker, NULL) == 0);
    gt_run_on(nw);

    long expected = (long)LOCKERS * BUMPS;
    printf("counter: %ld (expected %ld)\n", counter, expected);
    assert(counter == expected && "gt_mutex failed to exclude");

    /* --- channel --- */
    gt_chan_init(&chan);
    for (long i = 0; i < PRODUCERS; i++) assert(gt_create(producer, (void *)i) == 0);
    for (long i = 0; i < CONSUMERS; i++) assert(gt_create(consumer, NULL) == 0);
    assert(gt_create(closer, NULL) == 0);
    gt_run_on(nw);

    long total = PRODUCERS * PER_PRODUCER;
    /* Values sent were 1..total across all producers, each exactly once. */
    long expected_sum = 0;
    for (long i = 0; i < total; i++) expected_sum += i + 1;

    printf("produced %ld, consumed %ld, checksum %ld (expected %ld)\n", produced,
           consumed, sum_received, expected_sum);
    assert(produced == total);
    assert(consumed == total && "a message was lost or duplicated");
    assert(sum_received == expected_sum && "channel corrupted a value");

    printf("OK: mutex excluded and channel delivered every message, %d workers\n", nw);
    return 0;
}
