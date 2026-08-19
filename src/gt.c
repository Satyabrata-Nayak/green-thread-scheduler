#include "gt.h"

#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

typedef enum { GT_READY, GT_RUNNING, GT_DEAD } gt_state;

typedef struct {
    ucontext_t ctx;
    char stack[GT_STACK_SIZE];
    gt_state state;
    void (*fn)(void *);
    void *arg;
} gt_thread;

static gt_thread threads[GT_MAX_THREADS];
static int thread_count = 0;
static int current = -1;   /* index of the thread currently running */
static int rr_cursor = 0;  /* round-robin search cursor */
static ucontext_t sched_ctx;

static void gt_trampoline(void) {
    gt_thread *t = &threads[current];
    t->fn(t->arg);
    t->state = GT_DEAD;
    swapcontext(&t->ctx, &sched_ctx);
}

int gt_create(void (*fn)(void *), void *arg) {
    if (thread_count >= GT_MAX_THREADS) return -1;

    gt_thread *t = &threads[thread_count];
    if (getcontext(&t->ctx) == -1) return -1;

    t->ctx.uc_stack.ss_sp = t->stack;
    t->ctx.uc_stack.ss_size = GT_STACK_SIZE;
    t->ctx.uc_link = &sched_ctx;
    t->fn = fn;
    t->arg = arg;
    t->state = GT_READY;

    makecontext(&t->ctx, gt_trampoline, 0);
    thread_count++;
    return 0;
}

void gt_yield(void) {
    if (current == -1) return; /* called from outside a green thread */
    gt_thread *t = &threads[current];
    t->state = GT_READY;
    swapcontext(&t->ctx, &sched_ctx);
    /* resumes here once the scheduler picks this thread again */
}

void gt_run(void) {
    int remaining = thread_count;
    while (remaining > 0) {
        /* find the next READY thread, round-robin from rr_cursor */
        int idx = -1;
        for (int i = 0; i < thread_count; i++) {
            int candidate = (rr_cursor + i) % thread_count;
            if (threads[candidate].state == GT_READY) {
                idx = candidate;
                break;
            }
        }
        if (idx == -1) break; /* nothing runnable (shouldn't happen without I/O) */

        rr_cursor = (idx + 1) % thread_count;
        current = idx;
        threads[idx].state = GT_RUNNING;
        swapcontext(&sched_ctx, &threads[idx].ctx);

        if (threads[idx].state == GT_DEAD) remaining--;
    }
    current = -1;
}
