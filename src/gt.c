#include "gt.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Two interchangeable context-switch backends. GT_UCONTEXT_SWITCH selects
   the portable ucontext one; the default is the hand-written x86-64 switch.
   Both are kept so the benchmark can measure one against the other. */

#ifdef GT_UCONTEXT_SWITCH

#include <ucontext.h>
typedef ucontext_t gt_ctx;

static inline void gt_switch(gt_ctx *from, gt_ctx *to) { swapcontext(from, to); }

static int gt_ctx_init(gt_ctx *ctx, char *stack, size_t size, void (*entry)(void)) {
    if (getcontext(ctx) == -1) return -1;
    ctx->uc_stack.ss_sp = stack;
    ctx->uc_stack.ss_size = size;
    ctx->uc_link = NULL; /* the entry point never returns */
    makecontext(ctx, entry, 0);
    return 0;
}

#else /* hand-written switch */

/* A context is just a stack pointer -- everything else lives on that stack. */
typedef void *gt_ctx;

extern void gt_switch_stack(void **save_sp, void *new_sp);

static inline void gt_switch(gt_ctx *from, gt_ctx *to) {
    gt_switch_stack((void **)from, *to);
}

/* Hand-build the stack frame that gt_switch_stack's epilogue expects, so the
   first switch into a new thread "returns" into its entry point:

       low                                                        high
       [ r15 r14 r13 r12 rbp rbx ] [ entry ]
       ^-- sp starts here          ^-- 16-byte aligned

   The six zeroed slots are popped, then ret consumes `entry`. Placing the
   return-address slot on a 16-byte boundary leaves rsp %16 == 8 on entry,
   which is exactly what the ABI guarantees a function after a call. */
static int gt_ctx_init(gt_ctx *ctx, char *stack, size_t size, void (*entry)(void)) {
    uintptr_t top = ((uintptr_t)(stack + size)) & ~(uintptr_t)15;
    top -= 16; /* headroom, keeps the slot 16-byte aligned */

    void **sp = (void **)top;
    *sp = (void *)entry;
    sp -= 6; /* r15, r14, r13, r12, rbp, rbx */
    memset(sp, 0, 6 * sizeof(void *));

    *ctx = sp;
    return 0;
}

#endif

typedef enum {
    GT_FREE = 0, /* slot never used, or reclaimable */
    GT_READY,
    GT_RUNNING,
    GT_BLOCKED, /* waiting for an fd to become ready */
    GT_DEAD
} gt_state;

typedef struct {
    gt_ctx ctx;
    char stack[GT_STACK_SIZE];
    int state; /* gt_state; int so the atomic builtins can CAS it */
    void (*fn)(void *);
    void *arg;
} gt_thread;

/* ------------------------------------------------------------------ *
 * Run queues
 *
 * One per worker. The owner pushes and pops at opposite ends (FIFO, so a
 * preempted thread goes to the back and round-robin fairness survives);
 * thieves steal from the tail, the end the owner will reach last, which
 * keeps the two away from each other most of the time.
 *
 * ponytail: mutex-protected ring, not a lock-free deque. At 16 workers the
 * lock is only contended when a steal actually happens, and steals are rare
 * once every worker has work. A Chase-Lev deque is the upgrade if profiling
 * ever says this lock matters.
 * ------------------------------------------------------------------ */

typedef struct {
    pthread_mutex_t lock;
    int items[GT_MAX_THREADS];
    int head;
    int count;
} gt_queue;

static void q_init(gt_queue *q) {
    pthread_mutex_init(&q->lock, NULL);
    q->head = 0;
    q->count = 0;
}

static void q_push(gt_queue *q, int v) {
    pthread_mutex_lock(&q->lock);
    q->items[(q->head + q->count) % GT_MAX_THREADS] = v;
    q->count++;
    pthread_mutex_unlock(&q->lock);
}

static int q_pop(gt_queue *q) { /* owner: take from the head */
    int v = -1;
    pthread_mutex_lock(&q->lock);
    if (q->count > 0) {
        v = q->items[q->head];
        q->head = (q->head + 1) % GT_MAX_THREADS;
        q->count--;
    }
    pthread_mutex_unlock(&q->lock);
    return v;
}

static int q_steal(gt_queue *q) { /* thief: take from the tail */
    int v = -1;
    pthread_mutex_lock(&q->lock);
    if (q->count > 0) {
        q->count--;
        v = q->items[(q->head + q->count) % GT_MAX_THREADS];
    }
    pthread_mutex_unlock(&q->lock);
    return v;
}

/* ------------------------------------------------------------------ *
 * Workers -- one per OS thread
 * ------------------------------------------------------------------ */

typedef struct {
    gt_ctx sched_ctx;
    gt_queue runq;
    int id;
    int current; /* green thread running here, -1 if none */
    volatile sig_atomic_t in_switch;
    timer_t timer;
    int has_timer;
    unsigned rand_state;
    long ran;    /* threads run here, for the work-stealing stats */
    long stolen; /* threads taken from another worker's queue */
    pthread_t os_thread;
} gt_worker;

static gt_thread threads[GT_MAX_THREADS];
static gt_worker workers[GT_MAX_WORKERS];
static int nworkers = 1;

/* Every worker reaches its own state through this, including from inside the
   timer signal handler -- which is why the per-worker in_switch flag has to
   live here rather than in a global. */
static __thread gt_worker *self = NULL;

static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_live = 0;     /* green threads created but not finished */
static int g_blocked = 0;  /* subset parked on an fd */
static int g_idle = 0;     /* workers currently finding nothing to do */
static int g_shutdown = 0; /* set when the last green thread dies */

static int epfd = -1;
static int wakefd = -1; /* eventfd, poked once to wake every idle worker */
#define GT_WAKE_TOKEN 0xFFFFFFFFu

/* Anything reached through `self` must be re-read after a context switch: a
   green thread can be stolen while it is parked, and resumes on a different
   worker than the one it left. Caching the worker pointer across a switch
   means writing to the wrong worker's state -- see gt_yield. */
static void gt_trampoline(void) {
    self->in_switch = 0;
    gt_thread *t = &threads[self->current];
    t->fn(t->arg);
    /* fn may have yielded any number of times, so self is not what it was. */
    t->state = GT_DEAD;
    self->in_switch = 1;
    gt_switch(&t->ctx, &self->sched_ctx);
}

int gt_create(void (*fn)(void *), void *arg) {
    /* ponytail: linear slot scan, O(GT_MAX_THREADS) per spawn. Fine at 128
       slots; swap in a free-list if the cap ever grows. */
    pthread_mutex_lock(&table_lock);
    int idx = -1;
    for (int i = 0; i < GT_MAX_THREADS; i++) {
        if (threads[i].state == GT_FREE || threads[i].state == GT_DEAD) {
            idx = i;
            break;
        }
    }
    if (idx == -1) {
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    gt_thread *t = &threads[idx];
    if (gt_ctx_init(&t->ctx, t->stack, GT_STACK_SIZE, gt_trampoline) == -1) {
        pthread_mutex_unlock(&table_lock);
        return -1;
    }
    t->fn = fn;
    t->arg = arg;
    t->state = GT_READY;
    __atomic_add_fetch(&g_live, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_unlock(&table_lock);

    /* Spawn onto the caller's own queue -- a green thread's children start
       where it is, and stealing moves them only if another worker is idle.
       Called from main before gt_run, there is no caller yet: use worker 0. */
    q_push(self ? &self->runq : &workers[0].runq, idx);
    return 0;
}

/* Every use of `self` here is deliberately a fresh read, and in_switch is
   raised before anything else. Both matter, and the second one subtly:
   gt_preempt calls this function, so a timer tick landing between entry and
   in_switch = 1 re-enters gt_yield on top of itself. The inner call can park
   the thread, let another worker steal it, and resume it elsewhere -- at
   which point a cached worker pointer in this frame would name the worker we
   LEFT, and the switch below would drive that worker's scheduler stack from
   this OS thread. Re-reading self makes the outer frame land on whichever
   worker actually owns the thread now. */
void gt_yield(void) {
    if (!self || self->current == -1) return; /* not on a green thread */
    self->in_switch = 1;

    gt_thread *t = &threads[self->current];
    t->state = GT_READY;
    gt_switch(&t->ctx, &self->sched_ctx);

    self->in_switch = 0;
}

/* Preemption is just an involuntary yield -- same state transition, same
   switch path, so there is only one switching mechanism to reason about. */
static void gt_preempt(int sig) {
    (void)sig;
    gt_worker *w = self;
    if (!w || w->in_switch) return;

    /* The kernel blocks SIGVTALRM for the duration of its own handler and
       lifts it again when the handler returns, via sigreturn. We are about to
       leave without returning -- the switch below resumes a different thread
       entirely -- so that automatic unblock would never happen and this would
       be the last preemption this worker ever saw.

       swapcontext() hides this by saving and restoring the signal mask on
       every switch, which is exactly the rt_sigprocmask syscall the
       hand-written switch exists to avoid. Lifting the block explicitly costs
       one syscall per timer tick (100/sec) instead of one per switch. */
    sigset_t vtalrm;
    sigemptyset(&vtalrm);
    sigaddset(&vtalrm, SIGVTALRM);
    sigprocmask(SIG_UNBLOCK, &vtalrm, NULL);

    gt_yield();
}

/* One timer per worker, not one per process: ITIMER_VIRTUAL would deliver to
   an arbitrary OS thread, so with several workers a busy thread could keep
   running while some unrelated worker absorbed its preemption. SIGEV_THREAD_ID
   aims each timer at the thread that armed it, and CLOCK_THREAD_CPUTIME_ID
   counts only that thread's own CPU time. */
static void gt_timer_start(gt_worker *w) {
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGVTALRM;
    /* glibc names this one _sigev_un._tid; there is no portable alias for it
       the way there is for sigev_notify_function. */
    sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);

    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &w->timer) == -1) {
        w->has_timer = 0;
        return;
    }

    struct itimerspec its;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = (long)GT_TIMESLICE_US * 1000L;
    its.it_interval = its.it_value;
    timer_settime(w->timer, 0, &its, NULL);
    w->has_timer = 1;
}

static void gt_timer_stop(gt_worker *w) {
    if (!w->has_timer) return;
    timer_delete(w->timer);
    w->has_timer = 0;
}

int gt_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Park the running thread until fd is ready for `events`.

   EPOLLONESHOT is what makes a shared epoll safe across workers: without it,
   a level-triggered event would wake every worker sitting in epoll_wait and
   each would queue the same green thread, putting one thread on several run
   queues at once. ONESHOT hands the event to exactly one of them.

   ponytail: one waiter per fd -- a second thread waiting on the same fd hits
   EPOLL_CTL_ADD EEXIST. A per-fd waiter list is the fix if that's ever
   needed. */
static int gt_wait(int fd, uint32_t events) {
    if (!self || self->current == -1) return -1; /* nothing to park */

    /* Raised before the registration, not after: everything from here to the
       switch has to be one indivisible step as far as the timer is concerned,
       or a preemption could flip this thread back to READY while epoll still
       has it registered as BLOCKED -- and could re-enter through gt_yield,
       for the reasons spelled out there. */
    self->in_switch = 1;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events | EPOLLONESHOT;
    ev.data.u32 = (uint32_t)self->current;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        self->in_switch = 0;
        return -1;
    }

    gt_thread *t = &threads[self->current];
    t->state = GT_BLOCKED;
    __atomic_add_fetch(&g_blocked, 1, __ATOMIC_ACQ_REL);
    gt_switch(&t->ctx, &self->sched_ctx);

    /* Woken by whichever worker saw the epoll event, which is rarely the one
       we parked on -- so every `self` below is a fresh read too. */
    self->in_switch = 0;

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

/* Wake every thread whose fd became ready and queue it on THIS worker --
   natural load balancing: whoever noticed the event does the work. */
static void gt_poll(gt_worker *w, int timeout_ms) {
    struct epoll_event evs[GT_MAX_THREADS];
    int n = epoll_wait(epfd, evs, GT_MAX_THREADS, timeout_ms);
    if (n < 0) {
        if (errno != EINTR) perror("epoll_wait");
        return;
    }
    for (int i = 0; i < n; i++) {
        uint32_t token = evs[i].data.u32;
        if (token == GT_WAKE_TOKEN) continue; /* shutdown poke */

        int idx = (int)token;
        if (idx < 0 || idx >= GT_MAX_THREADS) continue;

        /* CAS so that if two workers ever do see the same event, only one of
           them can queue the thread. EPOLLONESHOT should prevent it; this
           makes "should" into "cannot". */
        int expected = GT_BLOCKED;
        if (__atomic_compare_exchange_n(&threads[idx].state, &expected, GT_READY, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_sub_fetch(&g_blocked, 1, __ATOMIC_ACQ_REL);
            q_push(&w->runq, idx);
        }
    }
}

/* xorshift: the steal victim only needs to be arbitrary, and this keeps the
   choice thread-local instead of contending on rand()'s global state. */
static int gt_pick_victim(gt_worker *w) {
    unsigned x = w->rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    w->rand_state = x;
    return (int)(x % (unsigned)nworkers);
}

static int gt_steal_work(gt_worker *w) {
    if (nworkers < 2) return -1;
    /* One sweep: try random victims, and give up rather than spin here --
       the caller's idle path knows how to wait properly. */
    for (int tries = 0; tries < nworkers * 2; tries++) {
        int v = gt_pick_victim(w);
        if (v == w->id) continue;
        int idx = q_steal(&workers[v].runq);
        if (idx >= 0) {
            w->stolen++;
            return idx;
        }
    }
    return -1;
}

static void gt_wake_all(void) {
    uint64_t one = 1;
    if (wakefd >= 0) {
        ssize_t r = write(wakefd, &one, sizeof(one));
        (void)r;
    }
}

/* Nothing to run and nothing to steal. */
static void gt_idle(gt_worker *w) {
    int all_idle = (__atomic_add_fetch(&g_idle, 1, __ATOMIC_ACQ_REL) == nworkers);

    if (__atomic_load_n(&g_blocked, __ATOMIC_ACQUIRE) > 0) {
        /* Only sleep indefinitely once every worker is idle: while others are
           still running, work can appear in their queues, and epoll would not
           wake us for that. */
        gt_poll(w, all_idle ? -1 : 1);
    } else {
        /* No I/O pending, so the only possible source of work is another
           worker's queue. Yield the core and re-try the steal loop.

           ponytail: this spins while a worker is starved and others are busy.
           Bounded by how long the imbalance lasts, and it costs nothing once
           every worker has work. A condvar-based park/unpark is the fix if a
           workload ever idles here for long. */
        sched_yield();
    }

    __atomic_sub_fetch(&g_idle, 1, __ATOMIC_ACQ_REL);
}

static void gt_worker_loop(gt_worker *w) {
    for (;;) {
        int idx = q_pop(&w->runq);
        if (idx < 0) idx = gt_steal_work(w);

        if (idx < 0) {
            if (__atomic_load_n(&g_shutdown, __ATOMIC_ACQUIRE)) break;
            if (__atomic_load_n(&g_live, __ATOMIC_ACQUIRE) == 0) break;
            gt_idle(w);
            continue;
        }

#ifdef GT_DEBUG_SCHED
        {
            int expect = GT_READY;
            if (!__atomic_compare_exchange_n(&threads[idx].state, &expect, GT_RUNNING, 0,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                fprintf(stderr, "BUG worker %d popped thread %d in state %d\n", w->id,
                        idx, expect);
                abort();
            }
        }
#endif
        w->current = idx;
        w->ran++;
        threads[idx].state = GT_RUNNING;
        w->in_switch = 1; /* the resumed thread clears it */
        gt_switch(&w->sched_ctx, &threads[idx].ctx);
        w->current = -1;

        if (threads[idx].state == GT_READY) {
            q_push(&w->runq, idx); /* yielded or was preempted */
        } else if (threads[idx].state == GT_DEAD) {
            if (__atomic_sub_fetch(&g_live, 1, __ATOMIC_ACQ_REL) == 0) {
                __atomic_store_n(&g_shutdown, 1, __ATOMIC_RELEASE);
                gt_wake_all(); /* release any worker asleep in epoll_wait */
            }
        }
        /* GT_BLOCKED: epoll owns it now, it will be queued when its fd fires */
    }
}

static void *gt_worker_main(void *arg) {
    gt_worker *w = arg;
    self = w;
    gt_timer_start(w);
    gt_worker_loop(w);
    gt_timer_stop(w);
    return NULL;
}

static int gt_sched_setup(void) {
    if (epfd == -1) {
        epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd == -1) {
            perror("epoll_create1");
            return -1;
        }
        wakefd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakefd == -1) {
            perror("eventfd");
            return -1;
        }
        /* Level-triggered and never drained: one write releases every worker
           parked in epoll_wait and keeps releasing them, which is exactly
           what shutdown needs. */
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.u32 = GT_WAKE_TOKEN;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, wakefd, &ev) == -1) {
            perror("epoll_ctl wakefd");
            return -1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = gt_preempt;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGVTALRM, &sa, NULL);
    return 0;
}

void gt_run_on(int n) {
    if (n < 1) n = 1;
    if (n > GT_MAX_WORKERS) n = GT_MAX_WORKERS;
    if (gt_sched_setup() == -1) return;

    nworkers = n;
    __atomic_store_n(&g_shutdown, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_idle, 0, __ATOMIC_RELEASE);

    for (int i = 0; i < n; i++) {
        gt_worker *w = &workers[i];
        if (i > 0) q_init(&w->runq); /* worker 0's queue may already hold work */
        w->id = i;
        w->current = -1;
        w->in_switch = 0;
        w->has_timer = 0;
        w->ran = 0;
        w->stolen = 0;
        w->rand_state = 0x9E3779B9u ^ (unsigned)(i + 1);
    }

    for (int i = 1; i < n; i++) {
        if (pthread_create(&workers[i].os_thread, NULL, gt_worker_main, &workers[i]) != 0) {
            perror("pthread_create");
            nworkers = i; /* carry on with the workers we got */
            break;
        }
    }

    /* The calling thread is worker 0, so a single-worker run costs no
       pthread_create at all and behaves exactly as it did before. */
    self = &workers[0];
    gt_timer_start(&workers[0]);
    gt_worker_loop(&workers[0]);
    gt_timer_stop(&workers[0]);

    for (int i = 1; i < nworkers; i++) pthread_join(workers[i].os_thread, NULL);

    self = NULL;
    /* Drain the shutdown poke so a later gt_run starts from a clean slate. */
    uint64_t sink;
    while (wakefd >= 0 && read(wakefd, &sink, sizeof(sink)) > 0) {
    }
    __atomic_store_n(&g_shutdown, 0, __ATOMIC_RELEASE);
    nworkers = 1;
}

void gt_run(void) { gt_run_on(1); }

void gt_worker_stats(int worker, long *ran, long *stolen) {
    if (worker < 0 || worker >= GT_MAX_WORKERS) return;
    if (ran) *ran = workers[worker].ran;
    if (stolen) *stolen = workers[worker].stolen;
}

/* Called once before the first gt_create so worker 0's queue exists. */
__attribute__((constructor)) static void gt_boot(void) {
    for (int i = 0; i < GT_MAX_WORKERS; i++) q_init(&workers[i].runq);
    workers[0].current = -1;
}
