#include "gt.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

typedef enum {
    GT_FREE = 0, /* slot never used, or reclaimable */
    GT_READY,
    GT_RUNNING,
    GT_BLOCKED,  /* waiting for an fd to become ready */
    GT_DEAD
} gt_state;

typedef struct {
    ucontext_t ctx;
    char stack[GT_STACK_SIZE];
    gt_state state;
    void (*fn)(void *);
    void *arg;
} gt_thread;

static gt_thread threads[GT_MAX_THREADS];
static int nslots = 0;    /* high-water mark of slots ever used */
static int live = 0;      /* threads created but not yet finished */
static int blocked = 0;   /* subset of live[] parked on an fd */
static int current = -1;  /* index of the thread currently running */
static int rr_cursor = 0; /* round-robin search cursor */
static int epfd = -1;
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
    /* ponytail: linear slot scan, O(GT_MAX_THREADS) per spawn. Fine at 128
       slots; swap in a free-list if the cap ever grows. */
    int idx = -1;
    for (int i = 0; i < GT_MAX_THREADS; i++) {
        if (threads[i].state == GT_FREE || threads[i].state == GT_DEAD) {
            idx = i;
            break;
        }
    }
    if (idx == -1) return -1;

    gt_thread *t = &threads[idx];
    if (getcontext(&t->ctx) == -1) return -1;

    t->ctx.uc_stack.ss_sp = t->stack;
    t->ctx.uc_stack.ss_size = GT_STACK_SIZE;
    t->ctx.uc_link = &sched_ctx;
    t->fn = fn;
    t->arg = arg;
    t->state = GT_READY;

    makecontext(&t->ctx, gt_trampoline, 0);
    if (idx >= nslots) nslots = idx + 1;
    live++;
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
   SIGVTALRM, not SIGALRM), so the timer neither ticks while we are parked in
   epoll_wait nor interrupts blocking syscalls. */
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

int gt_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Park the running thread until fd is ready for `events`. The fd is
   registered level-triggered and removed again on wake, so a thread can
   never miss a readiness edge that arrived while it was descheduled.

   ponytail: one waiter per fd -- a second thread waiting on the same fd hits
   EPOLL_CTL_ADD EEXIST. A per-fd waiter list is the fix if that's ever
   needed. */
static int gt_wait(int fd, uint32_t events) {
    if (current == -1) return -1; /* not on a green thread: nothing to park */

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.u32 = (uint32_t)current;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) return -1;

    gt_thread *t = &threads[current];
    /* Registration, the state change and the switch must be atomic with
       respect to the timer: a preemption in the middle would flip this
       thread back to READY while it is registered as BLOCKED. */
    in_switch = 1;
    t->state = GT_BLOCKED;
    blocked++;
    swapcontext(&t->ctx, &sched_ctx);
    in_switch = 0;

    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    return 0;
}

ssize_t gt_read(int fd, void *buf, size_t n) {
    for (;;) {
        ssize_t r = read(fd, buf, n);
        if (r >= 0) return r;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        if (gt_wait(fd, EPOLLIN) == -1) return -1;
    }
}

ssize_t gt_write(int fd, const void *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, (const char *)buf + sent, n - sent);
        if (w > 0) {
            sent += (size_t)w;
            continue;
        }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        if (gt_wait(fd, EPOLLOUT) == -1) return -1;
    }
    return (ssize_t)sent;
}

int gt_accept(int listen_fd, struct sockaddr *addr, socklen_t *addrlen) {
    for (;;) {
        int fd = accept(listen_fd, addr, addrlen);
        if (fd >= 0) {
            if (gt_set_nonblocking(fd) == -1) {
                close(fd);
                return -1;
            }
            return fd;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        if (gt_wait(listen_fd, EPOLLIN) == -1) return -1;
    }
}

/* Wake every thread whose fd became ready. timeout_ms < 0 blocks until at
   least one does -- which is the whole point: the OS thread sleeps in the
   kernel instead of spinning, and wakes with work to do. */
static void gt_poll(int timeout_ms) {
    struct epoll_event evs[GT_MAX_THREADS];
    int n = epoll_wait(epfd, evs, GT_MAX_THREADS, timeout_ms);
    if (n < 0) {
        if (errno != EINTR) perror("epoll_wait");
        return;
    }
    for (int i = 0; i < n; i++) {
        int idx = (int)evs[i].data.u32;
        if (idx >= 0 && idx < nslots && threads[idx].state == GT_BLOCKED) {
            threads[idx].state = GT_READY;
            blocked--;
        }
    }
}

void gt_run(void) {
    if (epfd == -1) {
        epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd == -1) {
            perror("epoll_create1");
            return;
        }
    }

    in_switch = 1; /* scheduler bookkeeping is non-preemptible */
    gt_timer_start();

    while (live > 0) {
        /* find the next READY thread, round-robin from rr_cursor */
        int idx = -1;
        for (int i = 0; i < nslots; i++) {
            int candidate = (rr_cursor + i) % nslots;
            if (threads[candidate].state == GT_READY) {
                idx = candidate;
                break;
            }
        }

        if (idx == -1) {
            /* Nothing runnable. If threads are parked on I/O, sleep in the
               kernel until one is ready; otherwise nothing ever will be. */
            if (blocked == 0) break;
            gt_poll(-1);
            continue;
        }

        rr_cursor = (idx + 1) % nslots;
        current = idx;
        threads[idx].state = GT_RUNNING;
        in_switch = 1; /* the resumed thread clears it */
        swapcontext(&sched_ctx, &threads[idx].ctx);

        if (threads[idx].state == GT_DEAD) live--;
    }

    gt_timer_stop();
    current = -1;
    in_switch = 0;
}
