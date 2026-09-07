#ifndef GT_H
#define GT_H

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

#define GT_MAX_THREADS 128
#define GT_MAX_WORKERS 16
#define GT_STACK_SIZE (64 * 1024)
#define GT_TIMESLICE_US 10000 /* preemption interval, in CPU-time microseconds */

/* Create a new green thread running fn(arg). Returns 0 on success. Safe to
   call from inside a running green thread, so servers can spawn per-client
   threads while the scheduler is running. */
int gt_create(void (*fn)(void *), void *arg);

/* Voluntarily give up the CPU to the next READY thread. */
void gt_yield(void);

/* Run the scheduler until every green thread has finished. gt_run() uses a
   single OS thread; gt_run_on(n) spreads the work across n of them, each with
   its own run queue, stealing from the others when it runs dry. The calling
   thread is always worker 0. */
void gt_run(void);
void gt_run_on(int nworkers);

/* How many green threads a worker ran, and how many of those it stole from
   another worker's queue. For the scaling benchmark. */
void gt_worker_stats(int worker, long *ran, long *stolen);

/* I/O that parks only the calling green thread, never the OS thread. Same
   signatures as read/write/accept; the fd must be non-blocking. */
ssize_t gt_read(int fd, void *buf, size_t n);
ssize_t gt_write(int fd, const void *buf, size_t n);
int gt_accept(int listen_fd, struct sockaddr *addr, socklen_t *addrlen);

int gt_set_nonblocking(int fd);

/* Synchronisation that yields instead of blocking the OS thread. A
   pthread_mutex on contention would park the whole worker and every green
   thread queued behind it; these cost a context switch instead. */

#define GT_CHAN_CAP 64

typedef struct {
    int locked;
} gt_mutex;

#define GT_MUTEX_INIT {0}

void gt_mutex_init(gt_mutex *m);
void gt_mutex_lock(gt_mutex *m);
int gt_mutex_trylock(gt_mutex *m); /* 0 if acquired, -1 if already held */
void gt_mutex_unlock(gt_mutex *m);

/* Bounded channel. Senders wait while it is full, receivers while it is
   empty; a closed channel still drains before receives start failing. */
typedef struct {
    void *buf[GT_CHAN_CAP];
    int head;
    int count;
    int closed;
    gt_mutex lock;
} gt_chan;

void gt_chan_init(gt_chan *c);
int gt_chan_send(gt_chan *c, void *v);   /* 0 on success, -1 if closed */
int gt_chan_recv(gt_chan *c, void **out); /* 0 on success, -1 if closed+empty */
void gt_chan_close(gt_chan *c);

#endif
