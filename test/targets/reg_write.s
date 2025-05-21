.section .note.GNU-stack

.global main

.section .data
hex_format:         .asciz "%#x"
float_format:       .asciz "%.2f"
long_float_format:  .asciz "%.2Lf"

.section .text
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
    # save old value of rbp
    push %rbp
    movq %rsp, %rbp

    # getpid syscall (39)
    movq $39, %rax
    syscall
    # move the result to r12 (a register that wont be overwritten)
    movq %rax, %r12

    trap

    # Print contents of rsi
    leaq  hex_format(%rip), %rdi
    movq  $0, %rax
    call  printf@plt
    movq  $0, %rdi
    call  fflush@plt

    trap

    # print contents of mm0
    movq  %mm0, %rsi
    leaq  hex_format(%rip), %rdi
    movq  $0, %rax
    call  printf@plt
    movq  $0, %rdi
    call  fflush@plt

    trap

    # xmm0
    leaq float_format(%rip), %rdi
    movq $1, %rax
    # instead of moving the value into rsi,
    # we keep it in xmm0 and write 1 to rax
    # to tell printf that there is 1 vector argument
    call printf@plt
    movq $0, %rdi
    call fflush@plt

    trap

    # NOTE:
    # the SYSV ABI says that long double arguments must be passed on the function’s
    # stack frame rather than using registers,
    # so: manually push a value to the FPU stack from the
    # debugger, call fstp in our assembly code to pop the value from the FPU
    # stack onto our function stack, and then call printf with the long double specifier

    # allocate 16 bytes on the stack to store the contents of st0
    subq $16, %rsp
    # pop st0 from the stack
    fstpt (%rsp)
    leaq long_float_format(%rip), %rdi
    movq  $0, %rax
    call printf@plt
    movq $0, %rdi
    call fflush@plt
    # clean up the space allocated on the stack
    # (increment stack pointer to original position)
    addq $16, %rsp

    trap

    # restore
    popq %rbp
    # return 0
    movq $0, %rax
    ret

