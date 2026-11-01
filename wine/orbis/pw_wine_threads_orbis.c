/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The counters of the pthread layer linked into ntdll.prx, for the title's
 * lines. That layer (orbis-compat's pthread_create, MIT) gives every thread
 * that asks for less at least its floor, the main thread's stack size, and
 * counts the threads it created, the ones it raised and the creates that
 * failed. The title links its own copy of the layer, whose counters see
 * only the title's threads, so Wine's are read from ntdll.prx's copy through
 * this export (ntdll's descriptor list, tools/build_wine_orbis.sh). The
 * layer's functions are C++ (namespace orbis); they are named here by their
 * symbols. Linked into ntdll.prx only.
 *
 * A caller that sets its own stack address (spawn_thread: the thread's
 * kernel stack) is subject to the layer's caller-stack policy instead of the
 * floor, when the layer has one: keep (the stack is passed through), small
 * (a small stack of the layer's own) or floor (raised like any other). The
 * title sets it from WINE_PS5_THREAD_STACK before Wine starts
 * (pw_wine_thread_policy) and reads the counts back
 * (pw_wine_thread_caller_stacks). */
#include <stddef.h>
#include <string.h>

extern void pw_orbis_thread_counts(unsigned long *created, unsigned long *raised)
    __asm__("_ZN5orbis12threadCountsEPmS0_");
extern void pw_orbis_thread_failures(unsigned long *create_failed, unsigned long *attr_failed)
    __asm__("_ZN5orbis14threadFailuresEPmS0_");
/* The floor without resolving it (0 until a thread has taken the layer's
 * default path): threadStackFloor() would resolve it from the reader's own
 * stack, and under the keep policy Wine's threads never resolve it. */
extern size_t pw_orbis_thread_stack_floor(void) __asm__("_ZN5orbis20threadStackFloorPeekEv");
extern void pw_orbis_thread_caller_policy(int policy, size_t small_bytes)
    __asm__("_ZN5orbis23threadCallerStackPolicyEim");
extern void pw_orbis_thread_caller_stacks(unsigned long *kept, unsigned long *small, unsigned long *refused,
                                          int *policy)
    __asm__("_ZN5orbis18threadCallerStacksEPmS0_S0_Pi");

static const char *const policy_names[] = { "keep", "small", "floor" };

/* Threads created through the layer, how many of them got a larger stack
 * than they asked for, how many creates failed, and the floor in bytes (0
 * while no thread has resolved it; reading it does not). Any pointer may be
 * NULL. */
void pw_wine_thread_stats(unsigned long *created, unsigned long *raised, unsigned long *create_failed,
                          unsigned long long *floor_bytes)
{
    unsigned long attr_failed = 0;

    pw_orbis_thread_counts(created, raised);
    pw_orbis_thread_failures(create_failed, &attr_failed);
    if (floor_bytes) *floor_bytes = pw_orbis_thread_stack_floor();
}

/* Sets the layer's caller-stack policy from "keep", "small" or "floor" (NULL
 * or empty: keep) and returns the name of the policy in force afterwards; an
 * unknown name changes nothing. */
const char *pw_wine_thread_policy(const char *name)
{
    int policy = -1, now = 0;
    unsigned int i;

    if (!name || !*name) name = "keep";
    for (i = 0; i < sizeof(policy_names) / sizeof(policy_names[0]); i++)
        if (!strcmp(name, policy_names[i])) policy = i;
    if (policy >= 0) pw_orbis_thread_caller_policy(policy, 0);
    pw_orbis_thread_caller_stacks(NULL, NULL, NULL, &now);
    return now >= 0 && now < 3 ? policy_names[now] : "unknown";
}

/* Threads started on their caller's own stack, given the layer's small stack
 * instead (the small policy, or a keep that libkernel refused), how many
 * keeps were refused, and the policy in force. Any pointer may be NULL. */
void pw_wine_thread_caller_stacks(unsigned long *kept, unsigned long *small, unsigned long *refused,
                                  const char **policy)
{
    int now = 0;

    pw_orbis_thread_caller_stacks(kept, small, refused, &now);
    if (policy) *policy = now >= 0 && now < 3 ? policy_names[now] : "unknown";
}
