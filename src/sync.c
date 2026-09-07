/* Green-thread synchronisation.
 *
 * The point of these is what they DON'T do: a pthread_mutex would block the
 * OS thread on contention, taking every other green thread on that worker
 * down with it. These yield instead, so a contended lock costs a context
 * switch (~19ns) rather than a descheduled OS thread.
 *
 * They are built on nothing but the public API -- gt_yield plus C11-style
 * atomics -- so they need no access to scheduler internals. */

#include "gt.h"

void gt_mutex_init(gt_mutex *m) { __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE); }

/* ponytail: test-and-set with a yield, not a wait queue. A waiter burns one
   scheduler turn per attempt, which is the right trade while critical
   sections stay short -- no bookkeeping, and no risk of a lost wakeup. Swap
   in a FIFO wait queue if a lock is ever held long enough for the spinning to
   show up in a profile; that also buys fairness, which this has none of. */
void gt_mutex_lock(gt_mutex *m) {
    while (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE)) {
        gt_yield();
    }
}

int gt_mutex_trylock(gt_mutex *m) {
    return __atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE) ? -1 : 0;
}

void gt_mutex_unlock(gt_mutex *m) {
    __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
}

void gt_chan_init(gt_chan *c) {
    gt_mutex_init(&c->lock);
    c->head = 0;
    c->count = 0;
    c->closed = 0;
}

void gt_chan_close(gt_chan *c) {
    gt_mutex_lock(&c->lock);
    c->closed = 1;
    gt_mutex_unlock(&c->lock);
}

int gt_chan_send(gt_chan *c, void *v) {
    for (;;) {
        gt_mutex_lock(&c->lock);
        if (c->closed) {
            gt_mutex_unlock(&c->lock);
            return -1;
        }
        if (c->count < GT_CHAN_CAP) {
            c->buf[(c->head + c->count) % GT_CHAN_CAP] = v;
            c->count++;
            gt_mutex_unlock(&c->lock);
            return 0;
        }
        /* Full: drop the lock before yielding, or no receiver could ever
           drain it and this would deadlock the moment the buffer filled. */
        gt_mutex_unlock(&c->lock);
        gt_yield();
    }
}

int gt_chan_recv(gt_chan *c, void **out) {
    for (;;) {
        gt_mutex_lock(&c->lock);
        if (c->count > 0) {
            *out = c->buf[c->head];
            c->head = (c->head + 1) % GT_CHAN_CAP;
            c->count--;
            gt_mutex_unlock(&c->lock);
            return 0;
        }
        /* Closed and drained is the only case that ends a receive: a closed
           channel still hands out whatever is already buffered. */
        if (c->closed) {
            gt_mutex_unlock(&c->lock);
            return -1;
        }
        gt_mutex_unlock(&c->lock);
        gt_yield();
    }
}
