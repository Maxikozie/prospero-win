/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* exit(), _exit() and abort() of a Wine PRX on the PS4: see the header.
 * Linked with --wrap=exit --wrap=_exit --wrap=abort, so every call inside
 * the module reaches __wrap_*; __real_* are libc's. */
#include "pw_wine_exit_orbis.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

extern void __real_exit(int status) __attribute__((noreturn));
extern void __real__exit(int status) __attribute__((noreturn));
extern void __real_abort(void) __attribute__((noreturn));

static PwWineExitHook exit_hook;
static void *exit_context;
/* The hook is entered once per process, on the first thread that leaves;
 * that thread's later exit calls (the hook ending the process itself, or
 * the hook returning) take the real call. Other threads leaving meanwhile
 * wait for it. */
static volatile int exiting;
static pthread_t exiting_thread;

void pw_wine_set_exit_hook(PwWineExitHook hook, void *context)
{
    __atomic_store_n(&exit_context, context, __ATOMIC_RELEASE);
    __atomic_store_n(&exit_hook, hook, __ATOMIC_RELEASE);
}

void pw_wine_get_exit_hook(PwWineExitHook *hook, void **context)
{
    *hook = __atomic_load_n(&exit_hook, __ATOMIC_ACQUIRE);
    *context = __atomic_load_n(&exit_context, __ATOMIC_ACQUIRE);
}

void pw_wine_exit_dispatch(int status, const char *kind)
{
    PwWineExitHook hook = __atomic_load_n(&exit_hook, __ATOMIC_ACQUIRE);

    if (hook && !__atomic_exchange_n(&exiting, 1, __ATOMIC_ACQ_REL)) {
        exiting_thread = pthread_self();
        hook(__atomic_load_n(&exit_context, __ATOMIC_ACQUIRE), status, kind);
        /* The hook came back: the real call ends the process as before. */
    }
    else if (hook && !pthread_equal(pthread_self(), exiting_thread)) {
        /* Another thread is leaving through the hook: this one sleeps until
         * the process ends, so the hook sees one exit. */
        for (;;) sleep(60);
    }
    if (kind[0] == 'a') __real_abort();
    if (kind[0] == '_') __real__exit(status);
    __real_exit(status);
}

void __wrap_exit(int status) __attribute__((noreturn));
void __wrap_exit(int status)
{
    pw_wine_exit_dispatch(status, "exit");
    __real_exit(status);
}

void __wrap__exit(int status) __attribute__((noreturn));
void __wrap__exit(int status)
{
    pw_wine_exit_dispatch(status, "_exit");
    __real__exit(status);
}

void __wrap_abort(void) __attribute__((noreturn));
void __wrap_abort(void)
{
    pw_wine_exit_dispatch(PW_WINE_EXIT_STATUS_ABORT, "abort");
    __real_abort();
}
