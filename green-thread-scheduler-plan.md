# Green-Thread Scheduler — Phase-Wise Implementation Plan

## Project goal

Build an M:N green-thread scheduling library in C: many lightweight
userspace "green threads" multiplexed cooperatively/preemptively onto a
small number of real OS threads, with non-blocking I/O integration and
measured benchmarks against native `pthread`s.

**Why this project:** demonstrates OS-level concurrency and scheduling
understanding (relevant to software interviews) plus low-latency
runtime-design reasoning (relevant to quant infra interviews), without
requiring kernel-level development or specialized hardware.

**Environment:** WSL2 (Ubuntu), C (C11), GCC/Clang, GDB, Make.
Confirm WSL2 (not WSL1) before starting — run `wsl -l -v` in
PowerShell; the version column must read `2`. WSL1 has had gaps in
`epoll` support; WSL2 runs a real Linux kernel so `ucontext.h`,
`epoll`, `setitimer`/`SIGALRM`, and hand-written x86-64 assembly all
behave exactly as on native Linux.

**How to work with Claude Code, given this is from-scratch:**
- Treat each phase below as one Claude Code session with one clear
  goal. Don't ask it to "build the whole scheduler" in one prompt —
  you'll get code you can't defend in an interview.
- After Claude Code generates something, ask it to walk you through
  *why* each part works before moving on — especially the context
  switch and the scheduler loop. If you can't explain a function
  yourself, don't move to the next phase.
- Commit to git after every phase (`git commit -m "phase N: ..."`) so
  you always have a working checkpoint to roll back to — context
  switching bugs can be nasty (silent crashes, corrupted stacks) and
  you don't want to debug three phases of changes at once.
- Run under GDB and Valgrind from Phase 1 onward, not just at the end
  — stack/register bugs are far easier to catch early.
- Keep a `NOTES.md` where you jot, in your own words, what each phase
  taught you. This becomes your interview prep material and your
  README's design-rationale section.

---

## Minimal scope vs. extensions — the honest breakdown

| | Included | Time (with Claude Code) |
|---|---|---|
| **Minimal (must-have)** | Phases 0–2, 5, 7, 8 | ~2.5–3 weeks |
| **Strong extension** | + Phase 3 (asm context switch) | +3–4 days |
| **Full scope** | + Phase 4 (multi-core work-stealing), Phase 6 (sync primitives) | +1.5–2 weeks |

If your month gets squeezed, Phases 0, 1, 2, 5, 7, 8 alone still give
you a complete, benchmarked, defensible project. Phases 3, 4, 6 are
where you go from "solid" to "genuinely impressive" — attempt them in
that order, and it's fine to list any you don't reach as "future work"
in your README. That's normal and honest, not a weakness.

---

## Phase 0 — Environment setup & identification (Day 1)

**Goal:** a working, verified toolchain before writing a single line
of scheduler code.

**Tasks:**
- Confirm WSL2, update packages: `sudo apt update && sudo apt install build-essential gdb valgrind git`
- Check `perf` availability (`sudo apt install linux-tools-generic` — may not fully work under WSL2; fall back to `clock_gettime`-based timing if so, which you'll need anyway)
- Initialize git repo, basic folder structure: `src/`, `include/`, `bench/`, `tests/`, `docs/`
- Read (don't yet code) the two core references end to end:
  - **"Green threads explained"** (c9x.me/articles/gthreads) — intro + code0 — the clearest small-scale conceptual walkthrough
  - **Douglas W. Jones, "A Minimal User-Level Thread Package"** — short academic paper, gives you the vocabulary (ready/running/blocked states) you'll want in interviews

**Deliverable:** a repo that builds an empty `main.c` with `make`, and notes in `NOTES.md` on what a green thread is, in your own words.

---

## Phase 1 — Minimal core: single-OS-thread cooperative scheduler (Week 1)

**Goal:** the actual heart of the project — N green threads cooperatively scheduled on **one** OS thread, round-robin.

**What to build:**
- A `struct green_thread` holding a `ucontext_t`, a stack buffer, and a state enum (`READY`, `RUNNING`, `DEAD`)
- `gt_create()`, `gt_yield()`, `gt_exit()` — using `ucontext.h` (`getcontext`, `makecontext`, `swapcontext`)
- A simple round-robin scheduler loop that picks the next `READY` thread

**References:**
- **c9x.me/articles/gthreads (code0)** — use this as your primary template; it's ~200 lines, fully explained, and small enough to genuinely understand rather than copy
- **`prabhendu/operating_system_1`** (GitHub) — a second, independently-written reference for the same `ucontext` + round-robin approach, useful for cross-checking your understanding
- `man ucontext` — read the actual man page; interviewers may ask what `makecontext`/`swapcontext` do at the register level

**Use Claude Code for:** boilerplate (Makefile, struct scaffolding, basic test harness). **Do yourself:** actually tracing through `swapcontext` with GDB — step through a context switch instruction by instruction at least once. This is the single most interview-relevant exercise in the whole project.

**Deliverable:** a demo spawning 5–10 green threads that interleave output via `gt_yield()`, provably running on one OS thread (verify with `pthread_self()` or a global counter).

---

## Phase 2 — Preemption (Week 1–2)

**Goal:** stop relying on threads voluntarily yielding — force a switch on a timer, like a real OS scheduler does.

**What to build:**
- A `SIGALRM` handler wired via `setitimer(ITIMER_VIRTUAL, ...)` firing every ~10ms
- Inside the handler, trigger a context switch to the next `READY` thread (careful: signal handlers have restrictions — you cannot safely call `swapcontext` directly from an async signal handler in all cases; the standard workaround is to set a flag and check it, or use `sigsetjmp`/`siglongjmp` carefully — research this specifically, it's a well-known gotcha)

**References:**
- `man setitimer`, `man sigaction`
- Any of the coroutine libraries under GitHub's `stackful-coroutines` topic implement a preemptive variant — skim 1–2 for how they handle the signal-safety issue, but write your own

**Deliverable:** the same demo as Phase 1, but threads that *don't* call `gt_yield()` (e.g., a tight loop) still get preempted fairly.

---

## Phase 3 — Extension: hand-written x86-64 context switch (optional, strong differentiator)

**Goal:** replace `ucontext`'s slow, general-purpose context switch with a minimal hand-rolled one — this is your "10x faster" benchmark line.

**Reference:**
- **`jonnrb/gthread`** (GitHub, Rutgers OS coursework) — explicitly documents replacing `ucontext` with an asm switch "10x faster than `swapcontext()`," and explains why (`ucontext` saves/restores signal masks and more registers than strictly necessary)

**What to build:** a `switch_context(old_sp, new_sp)` function in inline/standalone x86-64 assembly that saves only callee-saved registers and the stack pointer, following the System V AMD64 calling convention.

**Deliverable:** a microbenchmark showing context-switch latency before (ucontext) vs. after (your asm version) — this becomes one of your best CV/interview numbers.

---

## Phase 4 — Extension: multi-core M:N with work-stealing (optional, full scope)

**Goal:** go from N:1 (all green threads on one OS thread) to true M:N — several OS threads (backed by `pthread_create`), each running its own scheduler, stealing work from each other's queues when idle.

**What to build:**
- Per-OS-thread run queues
- A simple work-stealing policy: when a scheduler's local queue is empty, it randomly picks another OS thread's queue and steals from the tail
- Basic synchronization on queues (a lock-free deque is the "correct" answer but a mutex-protected queue is a perfectly reasonable first version — note the simplification honestly)

**References:**
- **`qthreads`** (Sandia National Labs, referenced from `baruch/libwire`'s coroutine-library wiki) — a real M:N work-stealing runtime; read its design docs for the model, don't try to match its full feature set
- Go's runtime scheduler design (`GOMAXPROCS`, `G-M-P` model) — widely written about; good for describing *why* work-stealing helps in interviews, even if your implementation is simpler

**Deliverable:** the diagram we discussed earlier (queue → OS threads → CPU cores) made real — benchmark shows near-linear speedup on CPU-bound green-thread workloads as OS thread count increases toward core count.

---

## Phase 5 — I/O integration via epoll (Week 2–3, the hard/interesting part)

**Goal:** when a green thread does a blocking-style read (e.g., a socket read with no data yet), don't block the OS thread — switch to another green thread and resume this one when data's ready. This is the part that makes it *useful*, not just a toy.

**What to build:**
- An `epoll` instance per scheduler
- A green-thread-aware `gt_read()`/`gt_write()` wrapper: attempt a non-blocking read; on `EAGAIN`, register the fd with `epoll_ctl`, mark the thread `BLOCKED`, yield; the scheduler's main loop calls `epoll_wait` when no threads are `READY`, and un-blocks threads whose fds became ready

**References:**
- **`tidwall/sco`** (GitHub) — production-quality coroutine scheduler built specifically to power an epoll-based networking framework (`neco`); read its scheduler/epoll integration for the pattern, but write your own — don't fork it (it's too complete to meaningfully "add" to)
- `man epoll` — specifically `EPOLLET` (edge-triggered) vs level-triggered semantics; know the difference, it's a common follow-up question

**Deliverable:** a toy TCP echo server built on your green-thread library, handling many concurrent connections on few OS threads — demoable and visually convincing.

---

## Phase 6 — Extension: basic synchronization primitives (optional)

**Goal:** a green-thread-aware mutex and a simple channel (Go-style), so green threads can safely share data without blocking the whole OS thread on a real `pthread_mutex`.

**What to build:** a `gt_mutex_lock()` that, on contention, yields instead of spinning or blocking the OS thread; a bounded channel with `gt_send`/`gt_recv`.

**Reference:** the "Cooperative Task Management" USENIX 2002 paper (Microsoft, referenced from the libwire coroutine-library wiki) — discusses exactly this trade-off space.

---

## Phase 7 — Benchmarking (Week 3, mandatory)

**Goal:** the actual CV/interview numbers. Don't skip or rush this — it's what separates this project from a copy-pasted fork.

**What to measure:**
1. **Context-switch latency** — green thread (ucontext) vs. green thread (asm, if you did Phase 3) vs. `pthread` context switch. Use `clock_gettime(CLOCK_MONOTONIC, ...)` around tight yield loops, averaged over thousands of iterations.
2. **Memory footprint per unit of concurrency** — spawn 10k green threads vs. 10k `pthread`s, measure RSS (`/proc/self/status`, `VmRSS`) or use `valgrind --tool=massif`.
3. **Throughput under I/O-bound load** (if you did Phase 5) — your echo server's requests/sec and P50/P99 latency vs. a naive one-`pthread`-per-connection version, using a simple load generator (even a basic multi-client script, or `wrk` if applicable).
4. **Scaling with OS thread count** (if you did Phase 4) — throughput as you vary the number of backing OS threads from 1 to core count.

**Tools:** `clock_gettime`, `/usr/bin/time -v`, `valgrind --tool=massif`, `perf stat` (if it works under your WSL2 setup — note in your README if it doesn't and what you used instead).

**Deliverable:** a `bench/` directory with reproducible scripts and a results table/graph you can screenshot into your README and CV writeup.

---

## Phase 8 — Documentation & interview-readiness polish (last few days)

**Goal:** turn the code into something that reads as a real project, not a script dump.

**Tasks:**
- README: goal, architecture diagram (reuse the M:N model description), how to build/run, benchmark results with numbers, and an honest **"design trade-offs"** section — mention the real debate around M:N threading complexity (worth citing: the widely-discussed HN thread on N:M green threads "solving complexity with more complexity") and where your implementation simplifies things (e.g., mutex-protected queues instead of lock-free)
- List explicitly what's implemented vs. "future work" (e.g., if you skipped Phase 4 or 6)
- `NOTES.md` → turn into your personal interview cheat-sheet: be ready to explain, unprompted, what happens step-by-step during a context switch, why preemption needs signal-safety care, and how epoll integration avoids blocking

---

## Consolidated reference list

| Reference | Use for |
|---|---|
| c9x.me/articles/gthreads | Primary template, Phase 1 |
| `prabhendu/operating_system_1` (GitHub) | Cross-check, Phase 1 |
| Douglas W. Jones, "A Minimal User-Level Thread Package" | Vocabulary/concepts, Phase 0 |
| `man ucontext`, `man setitimer`, `man sigaction`, `man epoll` | Ground truth on every syscall you use |
| `jonnrb/gthread` (GitHub) | Fast asm context switch, Phase 3 |
| `qthreads` (Sandia) / Go's G-M-P model writeups | Work-stealing design, Phase 4 |
| `tidwall/sco` (GitHub) | epoll integration pattern (read, don't fork), Phase 5 |
| "Cooperative Task Management" (USENIX 2002, Microsoft) | Sync primitive trade-offs, Phase 6 |
| HN discussion on N:M/green threads | Honest trade-offs section, Phase 8 |
