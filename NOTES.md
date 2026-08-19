# NOTES

Interview-prep journal. Write what each phase taught you in your own
words — this becomes the README's design-rationale section later.

## Phase 0 — Environment & concepts

- What is a green thread, in your own words?
- Ready / running / blocked states — where do they show up in the code?

## Phase 1 — Cooperative round-robin core

- What does `swapcontext` actually do at the register level? (step through in GDB)
- Why does the trampoline need a global `current` index instead of passing args via `makecontext`?
- What happens if a thread never calls `gt_yield()`? (this is exactly what Phase 2 fixes)
