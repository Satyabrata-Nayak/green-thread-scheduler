#ifndef GT_H
#define GT_H

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

#define GT_MAX_THREADS 128
#define GT_STACK_SIZE (64 * 1024)
#define GT_TIMESLICE_US 10000 /* preemption interval, in CPU-time microseconds */

/* Create a new green thread running fn(arg). Returns 0 on success. Safe to
   call from inside a running green thread, so servers can spawn per-client
   threads while the scheduler is running. */
int gt_create(void (*fn)(void *), void *arg);

/* Voluntarily give up the CPU to the next READY thread. */
void gt_yield(void);

/* Run the scheduler loop until every thread has finished. */
void gt_run(void);

/* I/O that parks only the calling green thread, never the OS thread. Same
   signatures as read/write/accept; the fd must be non-blocking. */
ssize_t gt_read(int fd, void *buf, size_t n);
ssize_t gt_write(int fd, const void *buf, size_t n);
int gt_accept(int listen_fd, struct sockaddr *addr, socklen_t *addrlen);

int gt_set_nonblocking(int fd);

#endif
