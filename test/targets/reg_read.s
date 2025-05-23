.section .note.GNU-stack

.global main

.section .data
my_double: .double 64.125

.section .text

# reuse the macro from reg_write.s
.macro trap
    # syscall for kill is 62
    movq $62, %rax
    # first arg expected in rdi, here the PID of the process to signal
    movq %r12, %rdi
    # signal id for SIGTRAP is 5 (second argument, that goes in rsi)
    movq $5, %rsi
    syscall
.endm

main:
    push %rbp
    movq %rsp, %rbp

    # getpid syscall (39)
    movq $39, %rax
    syscall
    # move the result to r12 (a register that wont be overwritten)
    movq %rax, %r12

    # store to r13
    movq $0xcafecafe, %r13
    trap

    # store to r13b
    movb $42, %r13b
    trap

    # store to mm0
    # to test MMX registers, we can't move directly from
    # AN X86 IMMEDIATE TO AN MMX REGISTER, SO WE FIRST STORE THE VALUE IN R13
    # then transfer to mm0
    movq $0xba5eba11, %r13
    movq %r13, %mm0
    trap

    # store to xmm0
    movsd my_double(%rip), %xmm0
    trap

    # store to st0
    emms # clear the MMX state, as MMX and x87 share registers
    # instruction to load a floating point number to the FPU stack
    fldl my_double(%rip)
    trap

    popq %rbp
    movq $0, %rax
    ret
