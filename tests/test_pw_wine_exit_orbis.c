/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The exit wraps of a Wine PRX on the PS4: a registered hook sees the
 * status and the kind of every way out and is entered once per process;
 * without one, or when it returns, the real call runs. Built with
 * --wrap=exit,_exit,abort so the test's own calls go through the wraps.
 * The single-entry flag is process-wide, so every scenario runs in a
 * forked child and reports through its exit status. */
#include "../wine/orbis/pw_wine_exit_orbis.h"
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static jmp_buf back;
static int seen_status, seen_calls, seen_context;
static char seen_kind[16];

static void hook(void *context, int status, const char *kind)
{
    seen_calls++;
    seen_status = status;
    seen_context = *(int *)context;
    snprintf(seen_kind, sizeof(seen_kind), "%s", kind);
    longjmp(back, 1);   /* a real hook never returns; the test unwinds instead */
}

static void returning_hook(void *context, int status, const char *kind)
{
    (void)context; (void)status; (void)kind;
    seen_calls++;
}

/* Runs FN in a child and returns its wait status. */
static int in_child(void (*fn)(void))
{
    int status = 0;
    pid_t pid = fork();

    assert(pid >= 0);
    if (!pid) { fn(); _exit(99); }
    assert(waitpid(pid, &status, 0) == pid);
    return status;
}

static int context = 42;

/* exit() reaches the hook with its status, kind and context; the real exit
 * after that (the flag is set) ends the child with 0 on success. */
static void child_exit_hooked(void)
{
    pw_wine_set_exit_hook(hook, &context);
    if (!setjmp(back)) exit(5);
    if (seen_calls != 1 || seen_status != 5 || strcmp(seen_kind, "exit") || seen_context != 42) _exit(1);
    _exit(0);
}
static void child_underscore_exit_hooked(void)
{
    pw_wine_set_exit_hook(hook, &context);
    if (!setjmp(back)) _exit(11);
    if (seen_status != 11 || strcmp(seen_kind, "_exit")) _exit(1);
    _exit(0);
}
static void child_abort_hooked(void)
{
    pw_wine_set_exit_hook(hook, &context);
    if (!setjmp(back)) abort();
    if (seen_status != PW_WINE_EXIT_STATUS_ABORT || strcmp(seen_kind, "abort")) _exit(1);
    _exit(0);
}
static void child_exit_without_hook(void) { exit(7); }
static void child_underscore_exit_without_hook(void) { _exit(8); }
static void child_abort_without_hook(void) { abort(); }
static void child_hook_cleared(void) { pw_wine_set_exit_hook(hook, &context); pw_wine_set_exit_hook(NULL, NULL); exit(6); }
/* A hook that returns hands over to the real exit. */
static void child_hook_returns(void) { pw_wine_set_exit_hook(returning_hook, NULL); exit(9); }

/* A second thread leaving while the hook runs waits; the hook's thread
 * ends the process with the first status. */
static void *second_leaver(void *arg)
{
    (void)arg;
    exit(50);
}
static void waiting_hook(void *context, int status, const char *kind)
{
    pthread_t thread;
    (void)context; (void)kind;
    assert(status == 40);
    assert(!pthread_create(&thread, NULL, second_leaver, NULL));
    usleep(100000);   /* the second thread is parked in the dispatch */
    _exit(seen_calls == 0 ? 0 : 1);   /* the real _exit: the flag is set */
}
static void child_two_threads(void) { pw_wine_set_exit_hook(waiting_hook, NULL); exit(40); }

int main(void)
{
    int status;

    {
        PwWineExitHook h = NULL; void *c = NULL;
        pw_wine_get_exit_hook(&h, &c);
        assert(!h && !c);
        pw_wine_set_exit_hook(hook, &context);
        pw_wine_get_exit_hook(&h, &c);
        assert(h == hook && c == &context);
        pw_wine_set_exit_hook(NULL, NULL);
    }
    status = in_child(child_exit_hooked);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    status = in_child(child_underscore_exit_hooked);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    status = in_child(child_abort_hooked);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    status = in_child(child_exit_without_hook);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    status = in_child(child_underscore_exit_without_hook);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 8);
    status = in_child(child_abort_without_hook);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    status = in_child(child_hook_cleared);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 6);
    status = in_child(child_hook_returns);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 9);
    status = in_child(child_two_threads);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("pw_wine_exit_orbis: ok\n");
    return 0;
}
