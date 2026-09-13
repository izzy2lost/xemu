/*
 * Android AArch64 coroutine backend.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Bionic signs sigsetjmp() return addresses with the thread's IA key, which
 * pthread_create() randomizes. QEMU disk I/O coroutines can migrate between
 * threads, so restoring those environments with siglongjmp() fails pointer
 * authentication. Switch our own native frames without using libc jmp_bufs.
 */

#include "qemu/osdep.h"
#include "qemu/coroutine_int.h"
#include "qemu/coroutine-tls.h"

#if !defined(__aarch64__)
#error "The Android coroutine backend requires AArch64"
#endif

#if defined(CONFIG_SAFESTACK) || defined(__ARM_FEATURE_PAC_DEFAULT)
#error "Migrating coroutine frames require a compatible stack/return-address ABI"
#endif

/* Layout shared with coroutine-android-aarch64.S. */
typedef struct {
    uint64_t x19_x30[12];
    uint64_t d8_d15[8];
    uint64_t fpcr;
    uint64_t fpsr;
} CoroutineFrame;

G_STATIC_ASSERT(sizeof(CoroutineFrame) == 176);
G_STATIC_ASSERT(offsetof(CoroutineFrame, d8_d15) == 96);
G_STATIC_ASSERT(offsetof(CoroutineFrame, fpcr) == 160);

typedef struct {
    Coroutine base;
    void *stack;
    size_t stack_size;
    void *sp;
} CoroutineAndroid;

QEMU_DEFINE_STATIC_CO_TLS(Coroutine *, current);
QEMU_DEFINE_STATIC_CO_TLS(CoroutineAndroid, leader);

int xemu_coroutine_swap(void **from_sp, void *to_sp, int action);

static void coroutine_trampoline(void)
{
    Coroutine *co = get_current();

    while (true) {
        co->entry(co->entry_arg);
        qemu_coroutine_switch(co, co->caller, COROUTINE_TERMINATE);
    }
}

Coroutine *qemu_coroutine_new(void)
{
    CoroutineAndroid *co = g_new0(CoroutineAndroid, 1);
    CoroutineFrame *frame;
    uintptr_t top;

    co->stack_size = COROUTINE_STACK_SIZE;
    co->stack = qemu_alloc_stack(&co->stack_size);
    top = ((uintptr_t)co->stack + co->stack_size) & ~(uintptr_t)15;
    frame = (CoroutineFrame *)(top - sizeof(*frame));
    memset(frame, 0, sizeof(*frame));
    frame->x19_x30[11] = (uintptr_t)coroutine_trampoline;
    asm volatile("mrs %0, fpcr" : "=r"(frame->fpcr));
    asm volatile("mrs %0, fpsr" : "=r"(frame->fpsr));
    co->sp = frame;
    return &co->base;
}

void qemu_coroutine_delete(Coroutine *co_)
{
    CoroutineAndroid *co = DO_UPCAST(CoroutineAndroid, base, co_);

    qemu_free_stack(co->stack, co->stack_size);
    g_free(co);
}

CoroutineAction __attribute__((noinline))
qemu_coroutine_switch(Coroutine *from_, Coroutine *to_, CoroutineAction action)
{
    CoroutineAndroid *from = DO_UPCAST(CoroutineAndroid, base, from_);
    CoroutineAndroid *to = DO_UPCAST(CoroutineAndroid, base, to_);

    set_current(to_);
    return xemu_coroutine_swap(&from->sp, to->sp, action);
}

Coroutine *qemu_coroutine_self(void)
{
    Coroutine *co = get_current();

    return co ? co : &get_ptr_leader()->base;
}

bool qemu_in_coroutine(void)
{
    Coroutine *co = get_current();

    return co && co->caller;
}
