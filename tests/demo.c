#include <pthread.h>
#include <stdio.h>

#include "gt.h"

static void worker(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < 3; i++) {
        printf("thread %ld: iter %d (OS thread %lu)\n", id, i, pthread_self());
        gt_yield();
    }
}

int main(void) {
    for (long i = 0; i < 8; i++) {
        gt_create(worker, (void *)i);
    }
    gt_run();
    printf("all green threads finished\n");
    return 0;
}
