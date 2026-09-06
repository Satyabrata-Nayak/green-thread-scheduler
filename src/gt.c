#include "gt.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
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

/* Set while a context switch or scheduler bookkeeping is in progress. The
   timer handler refuses to preempt during that window -- interrupting a
   half-completed swapcontext would corrupt the saved context. Cleared by
   whichever thread resumes, never by the scheduler, so all of gt_run runs
   non-preemptible. */
static volatile sig_atomic_t in_switch = 0;

static void gt_trampoline(void) {
    in_switch = 0;
    gt_thread *t = &threads[current];
    t->fn(t->arg);
    t->state = GT_DEAD;
    in_switch = 1;
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
    in_switch = 1;
    swapcontext(&t->ctx, &sched_ctx);
    /* resumes here once the scheduler picks this thread again */
    in_switch = 0;
}

/* Preemption is just an involuntary yield -- same state transition, same
   switch path, so there is only one switching mechanism to reason about. */
static void gt_preempt(int sig) {
    (void)sig;
    if (in_switch) return;
    gt_yield();
}

/* ITIMER_VIRTUAL counts CPU time consumed by this process (delivering
   SIGVTALRM, not SIGALRM), so the timer neither ticks while we are idle nor
   interrupts blocking syscalls -- which matters once epoll_wait is here. */
static void gt_timer_start(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = gt_preempt;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGVTALRM, &sa, NULL);

    struct itimerval tv;
    tv.it_value.tv_sec = 0;
    tv.it_value.tv_usec = GT_TIMESLICE_US;
    tv.it_interval = tv.it_value;
    setitimer(ITIMER_VIRTUAL, &tv, NULL);
}

static void gt_timer_stop(void) {
    struct itimerval off;
    memset(&off, 0, sizeof(off));
    setitimer(ITIMER_VIRTUAL, &off, NULL);
    signal(SIGVTALRM, SIG_IGN);
}

void gt_run(void) {
    int remaining = thread_count;
    in_switch = 1; /* scheduler bookkeeping is non-preemptible */
    gt_timer_start();
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
        in_switch = 1; /* the resumed thread clears it */
        swapcontext(&sched_ctx, &threads[idx].ctx);

        if (threads[idx].state == GT_DEAD) remaining--;
    }
    gt_timer_stop();
    current = -1;
    in_switch = 0;
}
