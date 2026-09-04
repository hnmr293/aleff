    .text

    .globl _aleff_unsafe_context_save
_aleff_unsafe_context_save:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x8, sp
    str x8, [x0, #96]
    mrs x8, fpcr
    mrs x9, fpsr
    str w8, [x0, #104]
    str w9, [x0, #108]
    stp d8, d9, [x0, #112]
    stp d10, d11, [x0, #128]
    stp d12, d13, [x0, #144]
    stp d14, d15, [x0, #160]
    mov w0, #0
    ret

    .globl _aleff_unsafe_context_restore
_aleff_unsafe_context_restore:
    ldp d8, d9, [x0, #112]
    ldp d10, d11, [x0, #128]
    ldp d12, d13, [x0, #144]
    ldp d14, d15, [x0, #160]
    ldr w8, [x0, #104]
    ldr w9, [x0, #108]
    msr fpcr, x8
    msr fpsr, x9
    ldr x8, [x0, #96]
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    mov sp, x8
    mov w0, #1
    ret

    .globl _aleff_unsafe_run_on_stack
_aleff_unsafe_run_on_stack:
    and x16, x0, #0xfffffffffffffff0
    mov sp, x16
    mov x0, x2
    blr x1
    brk #0

    .globl _aleff_unsafe_stack_copy
_aleff_unsafe_stack_copy:
    cbz x2, 2f
1:
    ldrb w3, [x1], #1
    strb w3, [x0], #1
    subs x2, x2, #1
    b.ne 1b
2:
    ret
