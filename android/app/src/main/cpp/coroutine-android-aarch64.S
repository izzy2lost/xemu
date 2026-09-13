/*
 * Native coroutine frames, independent of Bionic's per-thread jmp_buf ABI.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

.text
.p2align 2
.global xemu_coroutine_swap
.hidden xemu_coroutine_swap
.type xemu_coroutine_swap, %function
xemu_coroutine_swap:
    .cfi_startproc
    hint #34                       /* bti c */
    stp x19, x20, [sp, #-176]!
    .cfi_def_cfa_offset 176
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    .cfi_offset 29, -96
    .cfi_offset 30, -88
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mrs x3, fpcr
    mrs x4, fpsr
    stp x3, x4, [sp, #160]
    mov x3, sp
    str x3, [x0]

    /* x18 is Android's thread-local shadow call stack; never migrate it. */
    mov sp, x1
    ldp x19, x20, [sp]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x3, x4, [sp, #160]
    msr fpcr, x3
    msr fpsr, x4
    add sp, sp, #176
    .cfi_def_cfa_offset 0
    .cfi_restore 29
    .cfi_restore 30
    mov w0, w2
    ret
    .cfi_endproc
.size xemu_coroutine_swap, .-xemu_coroutine_swap

.section .note.GNU-stack,"",%progbits
