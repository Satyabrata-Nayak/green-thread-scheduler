/* epoll-driven load generator for the echo servers.
 *
 * The Python client this replaces was itself the bottleneck: one thread doing
 * synchronous round-trips can only ever have one request outstanding, so it
 * measured its own latency rather than the server's throughput. This keeps
 * every connection in flight at once on a single epoll loop, so the server is
 * the thing under load.
 *
 * Latency is measured per request -- the interval between writing a request
 * and reading its echo back -- and every sample is kept so the percentiles
 * are exact rather than estimated from buckets.
 *
 * Usage: ./bin/loadgen [port] [connections] [seconds] [msg_bytes] */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNS 512
#define MAX_MSG 4096
#define MAX_SAMPLES (8 * 1000 * 1000)

typedef struct {
    int fd;
    int want;        /* bytes still expected back for the current request */
    double sent_at;  /* when the current request was written */
    char buf[MAX_MSG];
} conn_t;

static conn_t conns[MAX_CONNS];
static double *samples;
static long nsamples = 0;
static long errors = 0;

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int connect_one(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

/* Write one request and start its clock. */
static int send_request(conn_t *c, size_t msg_bytes) {
    memset(c->buf, 'x', msg_bytes);
    c->sent_at = now_ns();
    c->want = (int)msg_bytes;

    size_t sent = 0;
    while (sent < msg_bytes) {
        ssize_t w = write(c->fd, c->buf + sent, msg_bytes - sent);
        if (w > 0) {
            sent += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 9000;
    int nconns = argc > 2 ? atoi(argv[2]) : 100;
    double seconds = argc > 3 ? atof(argv[3]) : 5.0;
    size_t msg_bytes = argc > 4 ? (size_t)atoi(argv[4]) : 64;

    if (nconns > MAX_CONNS) nconns = MAX_CONNS;
    if (msg_bytes > MAX_MSG) msg_bytes = MAX_MSG;

    samples = malloc(sizeof(double) * MAX_SAMPLES);
    if (!samples) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    int epfd = epoll_create1(0);
    int established = 0;
    for (int i = 0; i < nconns; i++) {
        int fd = connect_one(port);
        if (fd < 0) break;
        conns[established].fd = fd;

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.u32 = (uint32_t)established;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
        established++;
    }
    if (established == 0) {
        fprintf(stderr, "could not connect to port %d\n", port);
        return 1;
    }

    /* Prime every connection so they are all in flight from the start. */
    for (int i = 0; i < established; i++) send_request(&conns[i], msg_bytes);

    double start = now_ns();
    double deadline = start + seconds * 1e9;
    struct epoll_event evs[MAX_CONNS];

    while (now_ns() < deadline) {
        int n = epoll_wait(epfd, evs, MAX_CONNS, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            conn_t *c = &conns[evs[i].data.u32];
            char in[MAX_MSG];
            ssize_t r = read(c->fd, in, sizeof(in));
            if (r <= 0) {
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                errors++;
                continue;
            }
            c->want -= (int)r;
            if (c->want > 0) continue; /* echo split across reads */

            if (nsamples < MAX_SAMPLES) samples[nsamples++] = now_ns() - c->sent_at;
            if (now_ns() < deadline) send_request(c, msg_bytes);
        }
    }
    double elapsed = (now_ns() - start) / 1e9;

    for (int i = 0; i < established; i++) close(conns[i].fd);
    close(epfd);

    if (nsamples == 0) {
        fprintf(stderr, "no completed requests\n");
        return 1;
    }
    qsort(samples, (size_t)nsamples, sizeof(double), cmp_double);

    double total = 0;
    for (long i = 0; i < nsamples; i++) total += samples[i];

    printf("connections:  %d\n", established);
    printf("message size: %zu bytes\n", msg_bytes);
    printf("duration:     %.2f s\n", elapsed);
    printf("requests:     %ld\n", nsamples);
    printf("throughput:   %.0f req/s\n", nsamples / elapsed);
    printf("latency mean: %.1f us\n", total / nsamples / 1000.0);
    printf("latency p50:  %.1f us\n", samples[nsamples * 50 / 100] / 1000.0);
    printf("latency p99:  %.1f us\n", samples[nsamples * 99 / 100] / 1000.0);
    printf("latency p999: %.1f us\n", samples[nsamples * 999 / 1000] / 1000.0);
    printf("latency max:  %.1f us\n", samples[nsamples - 1] / 1000.0);
    if (errors) printf("errors:       %ld\n", errors);

    free(samples);
    return 0;
}
