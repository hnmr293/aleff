    .text

    .globl _aleff_unsafe_context_save
_aleff_unsafe_context_save:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    stmxcsr 64(%rdi)
    fnstcw 68(%rdi)
    xorl %eax, %eax
    ret

    .globl _aleff_unsafe_context_restore
_aleff_unsafe_context_restore:
    ldmxcsr 64(%rdi)
    fldcw 68(%rdi)
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movq 56(%rdi), %rdx
    movl $1, %eax
    jmp *%rdx

    .globl _aleff_unsafe_run_on_stack
_aleff_unsafe_run_on_stack:
    movq %rdi, %rsp
    andq $-16, %rsp
    movq %rdx, %rdi
    call *%rsi
    ud2

    .globl _aleff_unsafe_stack_copy
_aleff_unsafe_stack_copy:
    movq %rdx, %rcx
    rep movsb
    ret
