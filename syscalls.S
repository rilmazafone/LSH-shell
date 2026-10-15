#if !defined(__aarch64__)
#error "syscalls.S is arm64-only: LSH issues raw Darwin syscalls on Apple silicon"
#endif

#include <sys/syscall.h>

.text
.p2align 2

_lsh_syscall_fail:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    stp     x0, x1, [sp, #-16]!
    bl      ___error
    ldr     w1, [sp]
    str     w1, [x0]
    ldp     x0, x1, [sp], #16
    ldp     x29, x30, [sp], #16
    mov     x0, #-1
    ret

.macro SYSCALL name, number
    .globl  _\name
    .p2align 2
_\name:
    mov     x16, #\number
    svc     #0x80
    b.cc    1f
    b       _lsh_syscall_fail
1:  ret
.endm

.globl  _sys_exit
.p2align 2
_sys_exit:
    and     x0, x0, #0xff
    mov     x16, #SYS_exit
    svc     #0x80
    ret

SYSCALL sys_read,     SYS_read
SYSCALL sys_write,    SYS_write
SYSCALL sys_open,     SYS_open
SYSCALL sys_close,    SYS_close
SYSCALL sys_lseek,    SYS_lseek
SYSCALL sys_chdir,    SYS_chdir
SYSCALL sys_getpid,   SYS_getpid
SYSCALL sys_kill,     SYS_kill
