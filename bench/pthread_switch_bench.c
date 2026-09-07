/* pthread context-switch baseline.
 *
 * Two OS threads ping-pong through a pair of semaphores. Both are pinned to
 * the SAME core, because otherwise they would run side by side on two cores
 * and the measurement would be semaphore latency rather than a context
 * switch -- the kernel never has to swap anything.
 *
 * Accounting is deliberately identical to bench/switch_bench.c: one round
 * trip hands the CPU over twice (A -> B, B -> A), so the per-switch figure
 * divides by 2 * rounds in both benchmarks and the two numbers are directly
 * comparable.
 *
 * Usage: ./bin/pthread_switch_bench */

/* _GNU_SOURCE comes from the Makefile; pthread_setaffinity_np needs it. */
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <time.h>

#define ROUNDS 200000L
#define WARMUP 5000L

static sem_t to_b, to_a;
static long rounds = 0;

static void pin_to_cpu0(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static void *thread_b(void *arg) {
    (void)arg;
    pin_to_cpu0();
    for (long i = 0; i < rounds; i++) {
        sem_wait(&to_b);
        sem_post(&to_a);
    }
    return NULL;
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static double run_pass(long n) {
    rounds = n;
    sem_init(&to_b, 0, 0);
    sem_init(&to_a, 0, 0);

    pthread_t b;
    pthread_create(&b, NULL, thread_b, NULL);
    pin_to_cpu0();

    double start = now_ns();
    for (long i = 0; i < n; i++) {
        sem_post(&to_b);
        sem_wait(&to_a);
    }
    double elapsed = now_ns() - start;

    pthread_join(b, NULL);
    sem_destroy(&to_b);
    sem_destroy(&to_a);
    return elapsed;
}

int main(void) {
    run_pass(WARMUP); /* discarded: thread creation, first-touch, cache */

    double elapsed = run_pass(ROUNDS);
    long switches = ROUNDS * 2; /* A -> B and B -> A */

    printf("backend:        pthread + semaphore (both pinned to cpu0)\n");
    printf("round trips:    %ld\n", ROUNDS);
    printf("thread switches:%ld\n", switches);
    printf("elapsed:        %.3f ms\n", elapsed / 1e6);
    printf("per round trip: %.1f ns\n", elapsed / (double)ROUNDS);
    printf("per switch:     %.1f ns\n", elapsed / (double)switches);
    return 0;
}
