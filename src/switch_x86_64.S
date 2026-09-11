/* void gt_switch_stack(void **save_sp, void *new_sp);
 *
 * Minimal x86-64 context switch, System V AMD64 calling convention:
 *   rdi = where to store the outgoing stack pointer
 *   rsi = stack pointer to resume
 *
 * A "context" here is nothing but a stack pointer. The ABI already says the
 * caller keeps its own volatile registers alive across a call, so the only
 * state a switch must preserve is the six callee-saved registers plus the
 * return address the ret instruction consumes -- and all of that lives on the
 * stack we are swapping. Change rsp and you have changed threads.
 *
 * This is the whole reason it beats swapcontext(): glibc's version also
 * saves and restores the signal mask, which costs an rt_sigprocmask syscall
 * on every single switch. No syscall here -- it never leaves userspace.
 *
 * ponytail: does not save MXCSR or the x87 control word (rounding modes, FP
 * exception masks), which the ABI does call callee-saved. Threads in one
 * program share FP config in practice; add stmxcsr/fnstcw around the swap if
 * a thread ever needs its own rounding mode.
 */

    .text
    .globl gt_switch_stack
    .type  gt_switch_stack, @function
    .align 16

gt_switch_stack:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq  %rsp, (%rdi)      /* save outgoing stack pointer */
    movq  %rsi, %rsp        /* <- the context switch, one instruction */

    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbp
    popq  %rbx
    ret                     /* jumps to whatever that stack was about to do */

    .size gt_switch_stack, .-gt_switch_stack

/* Tell the linker this object needs no executable stack. */
    .section .note.GNU-stack,"",@progbits
