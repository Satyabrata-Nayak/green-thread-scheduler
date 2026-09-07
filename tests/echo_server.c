/* TCP echo server: one green thread per connection, all of them on a single
   OS thread. The acceptor green thread parks on the listening socket; each
   connection thread parks on its own socket. Whenever every thread is parked,
   the scheduler sleeps in epoll_wait rather than spinning.

   With more than one worker the connections are spread over that many OS
   threads instead, stealing from each other as load allows.

   Usage: ./bin/echo_server [port] [workers]   (default 9000, 1 worker) */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gt.h"

#define BUF_SIZE 4096

static int live_conns = 0;
static long total_conns = 0;

static void connection(void *arg) {
    int fd = (int)(long)arg;
    char buf[BUF_SIZE];

    for (;;) {
        ssize_t n = gt_read(fd, buf, sizeof(buf));
        if (n <= 0) break; /* 0 = peer closed, <0 = error */
        if (gt_write(fd, buf, (size_t)n) < 0) break;
    }

    close(fd);
    /* Atomic because with several workers these really are concurrent. */
    int live = __atomic_sub_fetch(&live_conns, 1, __ATOMIC_ACQ_REL);
    printf("[conn %d closed] live=%d total=%ld\n", fd, live,
           __atomic_load_n(&total_conns, __ATOMIC_ACQUIRE));
    fflush(stdout);
}

static void acceptor(void *arg) {
    int listen_fd = (int)(long)arg;

    for (;;) {
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int fd = gt_accept(listen_fd, (struct sockaddr *)&peer, &len);
        if (fd < 0) {
            perror("accept");
            break;
        }
        if (gt_create(connection, (void *)(long)fd) != 0) {
            fprintf(stderr, "out of green threads, dropping connection\n");
            close(fd);
            continue;
        }
        int live = __atomic_add_fetch(&live_conns, 1, __ATOMIC_ACQ_REL);
        long total = __atomic_add_fetch(&total_conns, 1, __ATOMIC_ACQ_REL);
        printf("[conn %d open]   live=%d total=%ld  (OS thread %lu)\n", fd, live, total,
               pthread_self());
        fflush(stdout);
    }
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 9000;
    int nworkers = argc > 2 ? atoi(argv[2]) : 1;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        return 1;
    }
    if (gt_set_nonblocking(listen_fd) == -1) {
        perror("fcntl");
        return 1;
    }

    printf("echo server on port %d, %d OS thread(s), main is %lu -- Ctrl-C to stop\n",
           port, nworkers, pthread_self());
    fflush(stdout);

    gt_create(acceptor, (void *)(long)listen_fd);
    gt_run_on(nworkers);

    close(listen_fd);
    return 0;
}
