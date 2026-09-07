# green-thread-scheduler

An M:N green-thread scheduler in C11 for Linux/x86-64. Many lightweight
userspace threads are multiplexed onto a small pool of OS threads, with
preemptive scheduling, work stealing, and `epoll`-backed non-blocking I/O.

Written from scratch: the context switch is hand-written x86-64 assembly, and
the scheduler, I/O layer and synchronisation primitives depend on nothing
beyond libc and the Linux syscall interface.

## Features

- **Cooperative and preemptive scheduling.** Threads yield explicitly, or are
  descheduled by a per-worker CPU-time timer after a 10ms slice.
- **Hand-written context switch.** 14 instructions, no syscall. A `ucontext`
  backend is retained for comparison and selected with `-DGT_UCONTEXT_SWITCH`.
- **M:N with work stealing.** Each OS thread runs its own scheduler over its
  own run queue and steals from the tail of another's when it runs dry.
- **Non-blocking I/O.** `gt_read`/`gt_write`/`gt_accept` park the calling green
  thread on `epoll` instead of blocking the OS thread.
- **Synchronisation.** A mutex and a bounded channel that yield on contention
  rather than parking the OS thread.
- **On-demand stacks.** 64KB per thread, mapped lazily with a `PROT_NONE`
  guard page below each one.

## Build and run

Requires Linux (x86-64), GCC or Clang, and GNU make.

```sh
make          # build everything into bin/
make check    # run the test suite
make bench    # context-switch comparison
```

A demo echo server, and a load generator to point at it:

```sh
./bin/echo_server 9000 1        # port, worker count
./bin/loadgen 9000 100 5 64     # port, connections, seconds, message bytes
```

## API

```c
int  gt_create(void (*fn)(void *), void *arg);   /* spawn a green thread   */
void gt_yield(void);                             /* yield the CPU          */
void gt_run(void);                               /* run on 1 OS thread     */
void gt_run_on(int nworkers);                    /* run on N OS threads    */

ssize_t gt_read (int fd, void *buf, size_t n);   /* parks the green thread */
ssize_t gt_write(int fd, const void *buf, size_t n);
int     gt_accept(int fd, struct sockaddr *addr, socklen_t *len);

void gt_mutex_lock(gt_mutex *m);                 /* yields on contention   */
int  gt_chan_send(gt_chan *c, void *v);          /* bounded channel        */
int  gt_chan_recv(gt_chan *c, void **out);
```

`gt_create` may be called from inside a running green thread, so a server can
spawn one thread per connection while the scheduler is running.

## Architecture

```
   green threads          run queues            OS threads         cores
   ───────────────        ──────────            ──────────         ─────
   G G G G G G G   ──►   [worker 0 deque] ──►   pthread 0    ──►    CPU
   G G G G G G G   ──►   [worker 1 deque] ──►   pthread 1    ──►    CPU
   G G G G G G G   ──►   [worker 2 deque] ──►   pthread 2    ──►    CPU
         │                      ▲
         │                      └── steal from tail when local queue empties
         │
         └── blocked on I/O ──► shared epoll (EPOLLONESHOT)
                                     │
                                     └── ready ──► queued on the worker
                                                   that observed the event
```

A green thread is a stack pointer, a stack, and a state
(`READY`/`RUNNING`/`BLOCKED`/`DEAD`). Switching is `movq %rsi, %rsp` with the
six callee-saved registers pushed and popped around it — everything else the
System V ABI already guarantees is dead across a call.

Threads never switch to each other directly; every switch goes
thread → scheduler → thread, which keeps scheduling policy in one place.

## Benchmarks

Measured on WSL2 (kernel 6.6.87, 16 cores), GCC 11, `-O2`. Reproduce with
`bench/run_all.sh`. Every figure below is a **median of repeated runs**, not a
best result; run-to-run spread is noted where it is material.

### Context-switch latency

Median of 7 runs, 2M switches per run.

| Backend | ns per switch | vs. `pthread` |
|---|---:|---:|
| Hand-written x86-64 | **28.0** | **157x faster** |
| `ucontext` / `swapcontext` | 533 | 8.3x faster |
| `pthread` + semaphore, both pinned to one core | 4,407 | baseline |

The gap between the two green-thread backends is one syscall.
`swapcontext` saves and restores the signal mask, which costs an
`rt_sigprocmask` on every switch; the hand-written path never enters the
kernel. Measured directly rather than inferred:

```
$ strace -c -e trace=rt_sigprocmask ./demo_ucontext   # 48 switches
    72 calls
$ strace -c -e trace=rt_sigprocmask ./bin/demo        # same workload
     0 calls
```

WSL2 syscalls are more expensive than on bare metal, so the multiple flatters
the assembly version. The syscall count is the claim that transfers.

### Memory per unit of concurrency

10,000 threads alive simultaneously, `VmRSS` and `VmSize` from
`/proc/self/status`:

| | Resident / thread | Virtual / thread | Total resident |
|---|---:|---:|---:|
| Green threads (64KB stacks) | **4.08 kB** | 68 kB | **42 MB** |
| `pthread`s, 64KB stacks | 8.23 kB | 68 kB | 84 MB |
| `pthread`s, default 8MB stacks | 8.27 kB | 8,196 kB | 84 MB / **82 GB virtual** |

Two separate effects, worth separating because they are often conflated:

- **Resident memory: ~2x.** Stacks are mapped lazily, so a thread that touches
  2KB occupies one page rather than sixteen. A `pthread` additionally carries
  kernel-side state (`task_struct`, kernel stack, TLS) that a green thread
  does not.
- **Address space: ~120x.** Default `pthread` stacks reserve 8MB each. 10,000
  of them reserve 82GB of virtual address space — which only works at all
  because Linux overcommits.

### Multi-core scaling

64 CPU-bound green threads, 20M iterations each. Median of 5 runs, with the
observed range, because this measurement is the noisiest here:

| Workers | Speedup (median) | Range over 5 runs | Efficiency |
|---:|---:|---:|---:|
| 2 | 1.41x | 1.19 – 1.62x | 71% |
| 4 | 2.75x | 2.64 – 3.20x | 69% |
| 6 | 3.80x | 3.60 – 4.81x | 63% |
| 8 | 4.83x | 4.74 – 5.75x | 60% |

Real but sublinear, and the spread is wide enough that any single run would
misrepresent it. Efficiency around 60–70% means roughly a third of each added
core is lost to coordination — plausibly the mutex on each run queue and the
shared thread table, with steal counts rising as worker count grows, showing
workers running dry more often. Measured under WSL2 on a machine running other
work, which accounts for some of the variance.

### I/O throughput and latency

100 concurrent connections, 64-byte echo, driven by `bench/loadgen.c`
(epoll-based, all connections in flight at once). Median of 5 runs:

| Server | Throughput | p50 | p99 |
|---|---:|---:|---:|
| Green threads, 1 worker | **50,258 req/s** | **1.93 ms** | **3.06 ms** |
| One `pthread` per connection | 17,369 req/s | 5.72 ms | 10.36 ms |

2.9x the throughput at 3.0x lower median latency and 3.4x lower p99 — one OS
thread against 100 of them. Run-to-run spread was under 5% for both servers.

Latency here is dominated by queueing: with 100 requests permanently in
flight, mean latency ≈ concurrency ÷ throughput, so the two columns are not
independent measurements.

**Adding workers makes I/O throughput worse, not better** — roughly 50,000
req/s at 1 worker, 36,000 at 2, 19,500 at 4, reproducible across runs. All
workers share one `epoll` instance, so concurrent `epoll_ctl` calls serialise
in the kernel, and green threads migrating between workers pull their stacks
across cores. At 64 bytes per request there is not enough work to pay for that
coordination.

The obvious alternative explanation — that idle workers poll with a 1ms
timeout instead of blocking — was tested by rebuilding with a 0ms timeout and
rejected: 19,721 vs 19,502 req/s, no material difference. The established fix
is a separate `epoll` instance per worker, as Go's netpoller and Tokio both
do; it is not implemented here.

## Design trade-offs

Deliberate simplifications, with what each would cost to remove:

| Simplification | Consequence | Upgrade path |
|---|---|---|
| Mutex-protected run queues | Contention above ~4 workers | Chase-Lev lock-free deque |
| One shared `epoll` instance | I/O throughput falls as workers rise | Per-worker `epoll` |
| Level-triggered `epoll`, register/deregister per wait | Two extra `epoll_ctl` per blocking op | `EPOLLET` with persistent registration and drain loops |
| Mutex is test-and-set plus yield | No fairness; a waiter can starve | FIFO wait queue |
| One waiter per fd | Second waiter on an fd gets `EEXIST` | Per-fd waiter list |
| Fixed thread table, linear slot scan | O(n) spawn; cap of `GT_MAX_THREADS` | Free list |
| Context switch omits `MXCSR`/x87 control word | A thread cannot hold its own rounding mode | `stmxcsr`/`fnstcw` around the swap |
| Idle worker spins on `sched_yield` when no I/O is pending | Burns a core while work is unevenly distributed | Condvar park/unpark |

Sites where a shortcut was taken deliberately are marked with a `ponytail:`
comment in the source.

Two further limits worth stating plainly: the assembly context switch is
x86-64 System V only, and the I/O layer is Linux-only by virtue of `epoll`.
Nothing here is portable to Windows or macOS without a second backend.

## Testing

`make check` runs five self-checking programs, each of which fails loudly
rather than printing something to be eyeballed:

| | |
|---|---|
| `demo` | cooperative round-robin on one OS thread |
| `preempt_demo` | threads that never yield are still descheduled fairly |
| `io_test` | a blocked read parks one green thread, not the OS thread |
| `mt_test` | 64 threads across N workers, every result exact, stealing observed |
| `sync_test` | mutual exclusion and channel delivery under parallelism |

`mt_test` and `sync_test` run at 1 and 4 workers, since several bugs during
development were invisible with a single worker.

Building with `-DGT_DEBUG_SCHED` adds a compare-and-swap in the scheduler that
aborts if a green thread is ever dispatched while not `READY` — the fastest
way to catch two workers running one thread. All tests pass clean under
Valgrind (0 errors, 0 leaks), on both context-switch backends.

## Layout

```
include/gt.h            public API
src/gt.c                scheduler, run queues, work stealing, epoll integration
src/switch_x86_64.S     context switch
src/sync.c              mutex and bounded channel
tests/                  self-checking tests and the echo server
bench/                  benchmarks, load generator, run_all.sh
```

## Not implemented

- Per-worker `epoll` instances (the fix for the I/O scaling result above)
- Lock-free run queues
- Timers, sleep, or deadline-based scheduling
- Any architecture other than x86-64, or any OS other than Linux
