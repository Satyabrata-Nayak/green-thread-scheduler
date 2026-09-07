/* Memory per unit of concurrency: N green threads vs N pthreads, all alive
 * at the same time.
 *
 * RSS is read from /proc/self/status, which reports resident pages -- what
 * the process actually occupies in RAM, as opposed to VmSize, which is
 * address space it has merely reserved. Both are printed because the gap
 * between them is the entire story: a pthread reserves 8MB of stack and
 * touches a few KB of it.
 *
 * Green threads and pthreads are measured in separate processes (select with
 * the mode argument) so neither pays for the other's peak.
 *
 * Usage: ./bin/memory_bench green|pthread|pthread-small [count] */

#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gt.h"

static long read_status_kb(const char *field) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, field, strlen(field)) == 0) {
            sscanf(line + strlen(field), " %ld", &kb);
            break;
        }
    }
    fclose(f);
    return kb;
}

static long baseline_rss, baseline_vm;
static int target_count;

static void report(const char *what, int alive) {
    long rss = read_status_kb("VmRSS:");
    long vm = read_status_kb("VmSize:");
    printf("%-22s alive=%-6d  RSS=%7ld kB (+%ld kB)  VmSize=%9ld kB\n", what, alive, rss,
           rss - baseline_rss, vm);
    if (alive > 0) {
        printf("%-22s per thread: %.2f kB resident, %.2f kB virtual\n", "",
               (double)(rss - baseline_rss) / alive, (double)(vm - baseline_vm) / alive);
    }
    fflush(stdout);
}

/* ---------------- green threads ---------------- */

static int green_alive = 0;
static volatile int green_stop = 0;

static void green_body(void *arg) {
    (void)arg;
    __atomic_add_fetch(&green_alive, 1, __ATOMIC_ACQ_REL);
    /* Stay alive, and keep touching the stack lightly, until told to stop. */
    while (!__atomic_load_n(&green_stop, __ATOMIC_ACQUIRE)) gt_yield();
}

static void green_monitor(void *arg) {
    (void)arg;
    while (__atomic_load_n(&green_alive, __ATOMIC_ACQUIRE) < target_count) gt_yield();
    report("green threads", green_alive);
    __atomic_store_n(&green_stop, 1, __ATOMIC_RELEASE);
}

static int run_green(int n) {
    target_count = n;
    int created = 0;
    for (int i = 0; i < n; i++) {
        if (gt_create(green_body, NULL) != 0) break;
        created++;
    }
    if (created < n) {
        fprintf(stderr, "only created %d of %d green threads "
                        "(rebuild with -DGT_MAX_THREADS=%d)\n",
                created, n, n + 8);
        target_count = created;
    }
    gt_create(green_monitor, NULL);
    gt_run();
    return created;
}

/* ---------------- pthreads ---------------- */

static sem_t pt_ready, pt_go;

static void *pt_body(void *arg) {
    (void)arg;
    char touch[512];
    memset(touch, 1, sizeof(touch)); /* touch a little stack, like the green one */
    sem_post(&pt_ready);
    sem_wait(&pt_go);
    return touch[0] ? NULL : NULL;
}

static int run_pthreads(int n, size_t stack_size) {
    sem_init(&pt_ready, 0, 0);
    sem_init(&pt_go, 0, 0);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_size) pthread_attr_setstacksize(&attr, stack_size);

    pthread_t *ids = calloc((size_t)n, sizeof(pthread_t));
    int created = 0;
    for (int i = 0; i < n; i++) {
        if (pthread_create(&ids[i], &attr, pt_body, NULL) != 0) break;
        created++;
    }
    for (int i = 0; i < created; i++) sem_wait(&pt_ready);

    report(stack_size ? "pthreads (64KB stack)" : "pthreads (default stack)", created);
    if (created < n) fprintf(stderr, "only created %d of %d pthreads\n", created, n);

    for (int i = 0; i < created; i++) sem_post(&pt_go);
    for (int i = 0; i < created; i++) pthread_join(ids[i], NULL);
    free(ids);
    pthread_attr_destroy(&attr);
    return created;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "green";
    int n = argc > 2 ? atoi(argv[2]) : 10000;

    baseline_rss = read_status_kb("VmRSS:");
    baseline_vm = read_status_kb("VmSize:");
    printf("baseline               alive=0       RSS=%7ld kB              VmSize=%9ld kB\n",
           baseline_rss, baseline_vm);

    if (strcmp(mode, "green") == 0) {
        run_green(n);
    } else if (strcmp(mode, "pthread") == 0) {
        run_pthreads(n, 0); /* whatever the system default is, usually 8MB */
    } else if (strcmp(mode, "pthread-small") == 0) {
        run_pthreads(n, 64 * 1024); /* like-for-like with GT_STACK_SIZE */
    } else {
        fprintf(stderr, "usage: %s green|pthread|pthread-small [count]\n", argv[0]);
        return 2;
    }
    return 0;
}
