/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "qemu/osdep.h"
#include "qemu/coroutine_int.h"
#include <asm/hwcap.h>
#include <fenv.h>
#include <pthread.h>
#include <sys/auxv.h>
#include <sys/prctl.h>

enum { SWITCHES = 1000 };
static pthread_barrier_t barrier;
static Coroutine *test_co;
static int step;
static __thread int thread_number;

/* Standalone stack allocation keeps this backend test independent of the
 * emulator's devices and main loop. The app uses qemu_alloc_stack's guards. */
void *qemu_alloc_stack(size_t *size)
{
    return g_malloc(*size);
}

void qemu_free_stack(void *stack, size_t size)
{
    g_free(stack);
}

static int __attribute__((noinline)) current_thread_number(void)
{
    asm volatile("" ::: "memory");
    return thread_number;
}

static CoroutineAction enter(Coroutine *co)
{
    Coroutine *caller = qemu_coroutine_self();
    CoroutineAction result;

    co->caller = caller;
    result = qemu_coroutine_switch(caller, co, COROUTINE_ENTER);
    co->caller = NULL;
    g_assert(qemu_coroutine_self() == caller);
    return result;
}

static void nested_entry(void *opaque)
{
    g_assert(qemu_in_coroutine());
    g_assert(qemu_coroutine_self() == opaque);
}

static void migrating_entry(void *opaque)
{
    Coroutine *self = qemu_coroutine_self();
    uint64_t stack_values[64];
    double value = 1.25;

    g_assert(self == test_co);
    for (int j = 0; j < 64; j++) {
        stack_values[j] = UINT64_C(0x123456789abcdef0) + j;
    }
    g_assert_cmpint(fesetround(FE_DOWNWARD), ==, 0);
    for (int i = 0; i < SWITCHES; i++) {
        g_assert(qemu_in_coroutine());
        g_assert(qemu_coroutine_self() == self);
        g_assert_cmpint(current_thread_number(), ==, i % 2);
        g_assert_cmpint(fegetround(), ==, FE_DOWNWARD);
        asm volatile("" : "+w"(value) : "m"(stack_values) : "memory");
        g_assert(value == 1.25 + i * 0.5);
        for (int j = 0; j < 64; j++) {
            g_assert_cmpuint(stack_values[j], ==,
                             UINT64_C(0x123456789abcdef0) + j);
        }

        if (i % 20 == 0) {
            Coroutine *nested = qemu_coroutine_new();
            nested->entry = nested_entry;
            nested->entry_arg = nested;
            g_assert_cmpint(enter(nested), ==, COROUTINE_TERMINATE);
            qemu_coroutine_delete(nested);
        }

        value += 0.5;
        step++;
        g_assert_cmpint(qemu_coroutine_switch(self, self->caller,
                                             COROUTINE_YIELD), ==,
                        COROUTINE_ENTER);
    }
}

static void *worker(void *opaque)
{
    thread_number = 1;
    /* Match the separate IA keys used by Android app threads, even when
     * this executable is launched directly by adb shell. */
    if (getauxval(AT_HWCAP) & HWCAP_PACA) {
        g_assert_cmpint(prctl(PR_PAC_RESET_KEYS, PR_PAC_APIAKEY,
                             0L, 0L, 0L), ==, 0);
    }
    for (int i = 0; i < SWITCHES / 2; i++) {
        pthread_barrier_wait(&barrier);
        g_assert_cmpint(enter(test_co), ==, COROUTINE_YIELD);
        g_assert(!qemu_in_coroutine());
        g_assert_cmpint(fegetround(), ==, FE_TONEAREST);
        pthread_barrier_wait(&barrier);
    }
    return NULL;
}

int main(void)
{
    pthread_t thread;

    g_assert(!qemu_in_coroutine());
    test_co = qemu_coroutine_new();
    test_co->entry = migrating_entry;
    g_assert_cmpint(pthread_barrier_init(&barrier, NULL, 2), ==, 0);
    g_assert_cmpint(pthread_create(&thread, NULL, worker, NULL), ==, 0);
    for (int i = 0; i < SWITCHES / 2; i++) {
        g_assert_cmpint(enter(test_co), ==, COROUTINE_YIELD);
        g_assert(!qemu_in_coroutine());
        g_assert_cmpint(fegetround(), ==, FE_TONEAREST);
        pthread_barrier_wait(&barrier);
        pthread_barrier_wait(&barrier);
    }
    g_assert_cmpint(pthread_join(thread, NULL), ==, 0);
    g_assert_cmpint(step, ==, SWITCHES);
    g_assert_cmpint(enter(test_co), ==, COROUTINE_TERMINATE);
    qemu_coroutine_delete(test_co);
    pthread_barrier_destroy(&barrier);
    puts("PASS: 1000 coroutine migrations, nested entry, stack/FP state, termination");
    return 0;
}
