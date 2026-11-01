/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host stand-in for the PS4 title's thread layer, preloaded into a host Wine
 * (tools/test_wine_ps5_driver.sh --preload, tests/test_wine_wincb.py). On the
 * PS4, the pthread_create linked into ntdll.prx replaces any requested stack
 * smaller than its floor (the main thread's stack size, 16 MiB in the title)
 * with a fresh attribute that carries only the detach state and the guard
 * size: a stack ADDRESS set with pthread_attr_setstack is dropped, and the
 * thread runs on a new stack of the floor's size. Wine's spawn_thread passes
 * its 1 MiB kernel stack that way, so every Wine thread but the main one runs
 * its Unix side somewhere else (measured: thread 002c's locals at 0x7e719bxxx,
 * its kernel stack at >= 0x1000000000).
 *
 * This library does the same on the host, written from that behaviour: when
 * the caller's attribute has a stack address, the thread is created with a
 * new attribute of $PW_FOREIGN_STACK_MIB MiB (default 16), or of
 * $PW_FOREIGN_STACK_KIB KiB when that is set (the layer's "small" policy
 * gives such a caller 512 KiB), and the caller's detach state and guard
 * size. On the PS4 the platform's stacks lie below
 * Wine's kernel stacks, and KeUserModeCallback's check compares addresses,
 * so the new stack is mapped in the first free range below the caller's
 * (MAP_FIXED_NOREPLACE, searched downward); when there is none, glibc's own
 * stack is used and the thread is counted as "above".
 * Every moved thread is counted, and the first one is reported on stderr
 * ("pw_foreign_stack: moved ..."); the totals are printed at exit. The
 * stacks are never unmapped: a test's few dozen threads cost address space
 * only. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

typedef int (*create_fn)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);

static atomic_ulong moved, above;

static void report(void)
{
    unsigned long n = atomic_load(&moved);
    if (n) fprintf(stderr, "pw_foreign_stack: %lu thread(s) moved off their stack, %lu of them above it\n", n,
                   atomic_load(&above));
}

/* SIZE bytes of stack below ADDR, or NULL: the first free 16 MiB-aligned
 * range under it, searched downward over at most 4 GiB. */
static void *map_below(void *addr, size_t size)
{
    uintptr_t step = (size + 0xffffff) & ~(uintptr_t)0xffffff;
    uintptr_t hint = ((uintptr_t)addr & ~(uintptr_t)0xffffff) - step;
    int tries;

    for (tries = 0; tries < 256 && hint >= step; tries++, hint -= step)
    {
        void *mem = mmap((void *)hint, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (mem == MAP_FAILED) continue;
        if (mem == (void *)hint) return mem;
        munmap(mem, size);   /* a kernel without MAP_FIXED_NOREPLACE took it as a hint */
    }
    return NULL;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    static create_fn real;
    static atomic_int registered;
    void *addr = NULL, *stack;
    size_t size = 0, guard = 0, bytes = (size_t)16 << 20;
    int detach = PTHREAD_CREATE_JOINABLE, ret;
    const char *env;
    pthread_attr_t fresh;

    if (!real && !(real = (create_fn)dlsym(RTLD_NEXT, "pthread_create"))) return 11; /* EAGAIN */
    if (!attr || pthread_attr_getstack(attr, &addr, &size) || !addr) return real(thread, attr, start, arg);

    if ((env = getenv("PW_FOREIGN_STACK_MIB")) && atoi(env) > 0) bytes = (size_t)atoi(env) << 20;
    if ((env = getenv("PW_FOREIGN_STACK_KIB")) && atoi(env) > 0) bytes = (size_t)atoi(env) << 10;
    if (pthread_attr_init(&fresh)) return real(thread, attr, start, arg);
    pthread_attr_getdetachstate(attr, &detach);
    pthread_attr_getguardsize(attr, &guard);
    pthread_attr_setdetachstate(&fresh, detach);
    pthread_attr_setguardsize(&fresh, guard);
    if ((stack = map_below(addr, bytes))) pthread_attr_setstack(&fresh, stack, bytes);
    else
    {
        pthread_attr_setstacksize(&fresh, bytes);
        atomic_fetch_add(&above, 1);
    }
    ret = real(thread, &fresh, start, arg);
    pthread_attr_destroy(&fresh);
    if (!ret && atomic_fetch_add(&moved, 1) == 0)
    {
        fprintf(stderr, "pw_foreign_stack: moved a thread off its stack %p+%#zx onto %zu KiB at %p\n",
                addr, size, bytes >> 10, stack);
        if (!atomic_exchange(&registered, 1)) atexit(report);
    }
    return ret;
}
