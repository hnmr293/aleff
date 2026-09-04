.code

PUBLIC aleff_unsafe_context_save
aleff_unsafe_context_save PROC
    mov qword ptr [rcx + 0], rbx
    mov qword ptr [rcx + 8], rbp
    mov qword ptr [rcx + 16], rdi
    mov qword ptr [rcx + 24], rsi
    mov qword ptr [rcx + 32], r12
    mov qword ptr [rcx + 40], r13
    mov qword ptr [rcx + 48], r14
    mov qword ptr [rcx + 56], r15
    lea rax, [rsp + 8]
    mov qword ptr [rcx + 64], rax
    mov rax, qword ptr [rsp]
    mov qword ptr [rcx + 72], rax
    stmxcsr dword ptr [rcx + 80]
    fnstcw word ptr [rcx + 84]
    movdqu xmmword ptr [rcx + 96], xmm6
    movdqu xmmword ptr [rcx + 112], xmm7
    movdqu xmmword ptr [rcx + 128], xmm8
    movdqu xmmword ptr [rcx + 144], xmm9
    movdqu xmmword ptr [rcx + 160], xmm10
    movdqu xmmword ptr [rcx + 176], xmm11
    movdqu xmmword ptr [rcx + 192], xmm12
    movdqu xmmword ptr [rcx + 208], xmm13
    movdqu xmmword ptr [rcx + 224], xmm14
    movdqu xmmword ptr [rcx + 240], xmm15
    xor eax, eax
    ret
aleff_unsafe_context_save ENDP

PUBLIC aleff_unsafe_context_restore
aleff_unsafe_context_restore PROC
    ldmxcsr dword ptr [rcx + 80]
    fldcw word ptr [rcx + 84]
    movdqu xmm6, xmmword ptr [rcx + 96]
    movdqu xmm7, xmmword ptr [rcx + 112]
    movdqu xmm8, xmmword ptr [rcx + 128]
    movdqu xmm9, xmmword ptr [rcx + 144]
    movdqu xmm10, xmmword ptr [rcx + 160]
    movdqu xmm11, xmmword ptr [rcx + 176]
    movdqu xmm12, xmmword ptr [rcx + 192]
    movdqu xmm13, xmmword ptr [rcx + 208]
    movdqu xmm14, xmmword ptr [rcx + 224]
    movdqu xmm15, xmmword ptr [rcx + 240]
    mov rbx, qword ptr [rcx + 0]
    mov rbp, qword ptr [rcx + 8]
    mov rdi, qword ptr [rcx + 16]
    mov rsi, qword ptr [rcx + 24]
    mov r12, qword ptr [rcx + 32]
    mov r13, qword ptr [rcx + 40]
    mov r14, qword ptr [rcx + 48]
    mov r15, qword ptr [rcx + 56]
    mov rsp, qword ptr [rcx + 64]
    mov rdx, qword ptr [rcx + 72]
    mov eax, 1
    jmp rdx
aleff_unsafe_context_restore ENDP

PUBLIC aleff_unsafe_run_on_stack
aleff_unsafe_run_on_stack PROC
    mov rsp, rcx
    and rsp, -16
    sub rsp, 32
    mov rcx, r8
    call rdx
    ud2
aleff_unsafe_run_on_stack ENDP

PUBLIC aleff_unsafe_stack_copy
aleff_unsafe_stack_copy PROC
    test r8, r8
    je done
copy_loop:
    mov r9b, byte ptr [rdx]
    mov byte ptr [rcx], r9b
    inc rcx
    inc rdx
    dec r8
    jne copy_loop
done:
    ret
aleff_unsafe_stack_copy ENDP

END
