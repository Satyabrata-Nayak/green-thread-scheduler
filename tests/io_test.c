/* Verifies that a green thread blocking on a read parks *itself* and not the
   OS thread: the reader blocks first, the writer must therefore get to run
   and produce the data that wakes the reader back up.

   If gt_read blocked the OS thread, the writer would never run and this test
   would hang forever instead of failing loudly -- so it is also the reason
   `make check` is worth running with a timeout. */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "gt.h"

static const char MSG[] = "hello over a socketpair";

static int sv[2];
static int reader_parked = 0;
static int writer_ran = 0;
static char got[64];
static ssize_t got_len = -1;

static void reader(void *arg) {
    (void)arg;
    reader_parked = 1; /* about to block: nothing is in the pipe yet */
    got_len = gt_read(sv[0], got, sizeof(got) - 1);
    if (got_len > 0) got[got_len] = '\0';
    /* Only reachable if the writer ran while we were parked. */
    assert(writer_ran && "reader woke without the writer ever running");
}

static void writer(void *arg) {
    (void)arg;
    /* Burn a few scheduler turns so the reader is definitely parked in
       epoll before any data exists to read. */
    for (int i = 0; i < 5; i++) gt_yield();
    assert(reader_parked && "reader had not reached gt_read yet");
    writer_ran = 1;
    ssize_t w = gt_write(sv[1], MSG, sizeof(MSG) - 1);
    assert(w == (ssize_t)(sizeof(MSG) - 1));
}

int main(void) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) {
        perror("socketpair");
        return 1;
    }
    assert(gt_set_nonblocking(sv[0]) == 0);
    assert(gt_set_nonblocking(sv[1]) == 0);

    /* Reader first, so it reaches gt_read and parks before the writer runs. */
    gt_create(reader, NULL);
    gt_create(writer, NULL);
    gt_run();

    printf("read back: \"%s\" (%zd bytes)\n", got, got_len);
    assert(got_len == (ssize_t)(sizeof(MSG) - 1));
    assert(strcmp(got, MSG) == 0);

    close(sv[0]);
    close(sv[1]);
    printf("OK: blocked read parked one green thread, not the OS thread\n");
    return 0;
}
