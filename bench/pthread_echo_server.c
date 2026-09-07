/* Thread-per-connection echo server: the comparison point for the
 * green-thread one. Same protocol, same buffer size, same socket options --
 * the only difference is that each connection gets a real OS thread and
 * blocking reads, which is the design green threads exist to replace.
 *
 * Usage: ./bin/pthread_echo_server [port]   (default 9100) */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 4096

static void *connection(void *arg) {
    int fd = (int)(long)arg;
    char buf[BUF_SIZE];

    pthread_detach(pthread_self());
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t w = write(fd, buf + sent, (size_t)(n - sent));
            if (w <= 0) goto done;
            sent += w;
        }
    }
done:
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 9100;

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

    printf("pthread echo server on port %d, one OS thread per connection\n", port);
    fflush(stdout);

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        pthread_t t;
        if (pthread_create(&t, NULL, connection, (void *)(long)fd) != 0) {
            fprintf(stderr, "pthread_create failed, dropping connection\n");
            close(fd);
        }
    }
}
