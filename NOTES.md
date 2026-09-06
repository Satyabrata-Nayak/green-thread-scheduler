# NOTES — design walkthrough & interview cheat-sheet

Running explanation of how this scheduler works and why each decision was
made. Becomes the README's design-rationale section later.

---

## Concepts

A **green thread** is a thread the kernel knows nothing about. It has its own
stack and its own saved CPU registers, but the decision of *which one runs
next* is made by our code in userspace, not by the kernel scheduler. The
kernel only ever sees one OS thread running one program.

Three states (vocabulary from Douglas Jones, "A Minimal User-Level Thread
Package"):

- `READY` — runnable, waiting for a turn
- `RUNNING` — holds the CPU right now
- `DEAD` — its function returned
- (`BLOCKED` arrives with epoll integration — waiting on I/O, not runnable)

The whole library is three functions: `gt_create`, `gt_yield`, `gt_run`.

---

## The API — `include/gt.h`

```c
#define GT_MAX_THREADS 128
#define GT_STACK_SIZE (64 * 1024)

int  gt_create(void (*fn)(void *), void *arg);
void gt_yield(void);
void gt_run(void);
```

**64KB stacks.** A `pthread` gets 8MB of stack by default, so each green
thread costs roughly **128x less memory**. That ratio is the whole
"concurrency density" benchmark, decided at design time.

**Fixed array, no `malloc`.** Static storage means the scheduler's data
structures can't fail an allocation or fragment mid-switch. This matters
once a signal handler is in the picture — `malloc` is not
async-signal-safe, so a scheduler that allocates cannot be preempted safely.

---

## Core state — `src/gt.c`

```c
typedef struct {
    ucontext_t ctx;          // saved CPU registers + stack pointer
    char stack[GT_STACK_SIZE];
    gt_state state;
    void (*fn)(void *);
    void *arg;
} gt_thread;

static gt_thread threads[GT_MAX_THREADS];
static int thread_count = 0;
static int current   = -1;  // who is running
static int rr_cursor = 0;   // where round-robin resumes searching
static ucontext_t sched_ctx;
```

`ctx` is where the CPU registers get parked when a thread isn't running.
`stack` is its private call stack — this is what makes it a *thread* rather
than a callback: it can be suspended deep inside a call chain and resumed
later with every local variable intact.

`sched_ctx` holds the scheduler's own context. **Key structural decision:
threads never switch directly to each other.** Every switch goes
thread → scheduler → next thread. That keeps scheduling policy in exactly
one place (`gt_run`) instead of smeared across every yield site.

`current` and `rr_cursor` are separate on purpose. `current` answers "who's
running?" (needed by `gt_yield` and the trampoline); `rr_cursor` answers
"where do I resume searching?" (needed for fairness). Merging them breaks
round-robin the moment a thread dies.

---

## The trampoline

```c
static void gt_trampoline(void) {
    gt_thread *t = &threads[current];
    t->fn(t->arg);
    t->state = GT_DEAD;
    swapcontext(&t->ctx, &sched_ctx);
}
```

Solves a real `makecontext` limitation: its variadic arguments are
`int`-sized, so passing a 64-bit `void *arg` portably means splitting it into
two `int`s and reassembling. Instead the trampoline takes **zero arguments**
and reads `fn`/`arg` out of `threads[current]`. Safe because the scheduler
sets `current` immediately before switching in.

It also guarantees every thread an exit path: when the user's `fn` returns,
control lands here, the thread marks itself `DEAD`, and switches back to the
scheduler, which never resumes it. Without this, a returning thread would
fall off the end of its stack into undefined behaviour.

---

## `gt_create` — building a frozen thread

```c
getcontext(&t->ctx);                    // snapshot a valid context to mutate
t->ctx.uc_stack.ss_sp   = t->stack;     // point it at OUR stack
t->ctx.uc_stack.ss_size = GT_STACK_SIZE;
t->ctx.uc_link          = &sched_ctx;   // fallback if fn returns
makecontext(&t->ctx, gt_trampoline, 0); // rewrite entry point
```

`getcontext` isn't there to save anything useful — `makecontext` requires a
fully-initialised `ucontext_t` (signal mask, FPU state) to modify, so you
snapshot the current context purely as a template, then overwrite the stack
and entry point.

`makecontext` writes the trampoline's address and a synthetic return address
onto the *new* stack, so the first `swapcontext` into `t->ctx` starts
executing `gt_trampoline` with `rsp` inside `t->stack`. Nothing runs at
create time — the thread is fully constructed but frozen.

`uc_link` is belt-and-braces: if the trampoline's explicit `swapcontext`
were ever bypassed, glibc still returns control to `sched_ctx` rather than
crashing.

---

## `gt_yield` — the heart of it

```c
void gt_yield(void) {
    if (current == -1) return;   // called from outside a green thread
    gt_thread *t = &threads[current];
    t->state = GT_READY;
    in_switch = 1;
    swapcontext(&t->ctx, &sched_ctx);
    in_switch = 0;               // resumes HERE when rescheduled
}
```

`swapcontext(&t->ctx, &sched_ctx)` does two things: saves the current CPU
state (callee-saved registers, stack pointer, instruction pointer, signal
mask) into `t->ctx`, then loads the scheduler's state and jumps.

**The mind-bending part:** `swapcontext` does not return when called. It
returns much later, when someone swaps back *into* `t->ctx` — execution then
resumes at the instruction after the call and `gt_yield` returns to its
caller as if nothing happened. From the thread's perspective `gt_yield()` was
just a slow function call. That illusion is what makes green threads usable.

The `current == -1` guard handles `gt_yield()` being called from `main`
rather than from a green thread; without it you'd index `threads[-1]`.

---

## `gt_run` — the scheduler loop

```c
while (remaining > 0) {
    int idx = -1;
    for (int i = 0; i < thread_count; i++) {
        int candidate = (rr_cursor + i) % thread_count;
        if (threads[candidate].state == GT_READY) { idx = candidate; break; }
    }
    if (idx == -1) break;

    rr_cursor = (idx + 1) % thread_count;
    current = idx;
    threads[idx].state = GT_RUNNING;
    in_switch = 1;
    swapcontext(&sched_ctx, &threads[idx].ctx);   // run the thread

    if (threads[idx].state == GT_DEAD) remaining--;
}
```

The scan starts at `rr_cursor`, not at 0 — that's what makes it round-robin
rather than "always favour thread 0". After picking `idx` the cursor advances
past it, so the next search starts with the following thread; `% thread_count`
wraps it into a circle.

`swapcontext(&sched_ctx, &threads[idx].ctx)` is the exact mirror of the one
in `gt_yield` — the scheduler parks itself and resumes the thread. The line
after it only executes when that thread yields or dies, which is why the
`GT_DEAD` check sits immediately after.

`remaining` counts live threads so the loop terminates when the last one
dies. The `idx == -1` break is a safety valve for "nothing is READY" —
unreachable today, but epoll integration will hit it constantly (all threads
BLOCKED on I/O), and that is exactly where `epoll_wait` goes.

---

## Preemption — timer-driven switching

Cooperative scheduling has one fatal flaw: a thread that never calls
`gt_yield()` (a tight compute loop) keeps the CPU forever and starves
everything else. A real OS scheduler solves this with a timer interrupt; we
solve it with a timer *signal*.

```c
setitimer(ITIMER_VIRTUAL, &tv, NULL);   // fires SIGVTALRM every 10ms

static void preempt_handler(int sig) {
    (void)sig;
    if (in_switch) return;   // don't interrupt a switch in progress
    gt_yield();              // involuntary yield
}
```

**Which timer.** `ITIMER_VIRTUAL` counts only CPU time actually consumed by
this process and delivers `SIGVTALRM` (not `SIGALRM` — that's `ITIMER_REAL`,
which counts wall-clock time). Virtual time is the right choice: it doesn't
tick while the process is idle or blocked in a syscall, so it won't spray
`EINTR` at the `epoll_wait` call later, and it measures the thing we actually
want to time-slice — CPU.

**The handler is one line, and that's the point.** Preemption is just an
*involuntary* `gt_yield()`. Same state transition, same context switch, same
code path — so there's only one switching mechanism in the codebase to reason
about and debug.

### The two signal-safety gotchas

**1. Reentrancy into the switch itself.** If the timer fires while a thread is
*already inside* `swapcontext` — halfway through saving its registers into
`t->ctx` — the handler would call `gt_yield` again and save the handler's
registers over the half-written context. Result: silent stack corruption,
the nastiest possible bug class here.

The fix is the `in_switch` flag: a `volatile sig_atomic_t` set before every
`swapcontext` and cleared at every point where a thread *resumes*
(`gt_yield` after its swap, and the top of `gt_trampoline`). The handler
checks it and returns immediately if a switch is in flight — that tick of the
time slice is simply lost, which is harmless.

Note the asymmetry: the scheduler sets `in_switch = 1` and never clears it.
The *resumed thread* clears it. That's deliberate — it means the whole of
`gt_run` (the queue scan, the bookkeeping) runs non-preemptible, which is
what you want, since a signal landing in the middle of the scheduler's own
data structures would corrupt them.

`volatile sig_atomic_t` matters: `volatile` stops the compiler caching it in
a register across the switch, and `sig_atomic_t` guarantees the load/store is
a single uninterruptible instruction. No mutex needed — signal delivery
happens on the same OS thread, at instruction boundaries, so a plain flag is
genuinely sufficient here.

**2. The signal mask.** The kernel blocks `SIGVTALRM` while its own handler
runs. `swapcontext` saves and restores the signal mask along with the
registers — so a context saved *inside* the handler carries "SIGVTALRM
blocked" with it. Naively you'd expect that thread to never be preempted
again.

It self-heals, and tracing why is worth understanding: the thread resumes
inside the handler frame, returns from `gt_yield`, returns from the handler,
and the C library's restorer invokes `rt_sigreturn`. The kernel then restores
the mask it saved on that thread's stack at *delivery* time — which had the
signal unblocked. So the block lasts only for the handler's remaining
instructions.

`SA_RESTART` is set on the handler so interrupted syscalls resume
automatically instead of returning `EINTR` — otherwise every blocking call in
the program would need manual retry logic.

### Known ceiling: stdio is not reentrant here

A green thread preempted in the middle of `printf` while another green thread
then calls `printf` can corrupt the stdio buffer. glibc's stream lock is
recursive *per OS thread*, and all our green threads share one OS thread, so
the lock does not protect us — it lets the second call straight in.

This is why the preemption demo has the threads compute silently and record
their scheduling into a shared trace array, with all printing done from
`main` after `gt_run` returns. The real fix is a green-thread-aware I/O layer
(the epoll work), which routes writes through the scheduler instead of
through raw stdio.

---

## Things to be able to explain unprompted

- What `swapcontext` does at the register level (step through it in GDB once).
- Why `ucontext` is slower than a hand-written switch: it makes a
  `sigprocmask` syscall on every switch to save/restore the signal mask, and
  saves more register state than the System V ABI strictly requires.
- Why preemption needs a critical-section flag, and what specifically
  corrupts without it.
- Why a fixed array and no `malloc` in the scheduler path.
- Why `ITIMER_VIRTUAL`/`SIGVTALRM` rather than `ITIMER_REAL`/`SIGALRM`.
