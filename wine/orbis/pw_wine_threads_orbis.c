/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The counters of the pthread layer linked into ntdll.prx, for the title's
 * lines. That layer (orbis-compat's pthread_create, MIT) gives every thread
 * that asks for less at least its floor, the main thread's stack size, and
 * counts the threads it created, the ones it raised and the creates that
 * failed. The title links its own copy of the layer, whose counters see
 * only the title's threads, so Wine's are read from ntdll.prx's copy through
 * this export (ntdll's descriptor list, tools/build_wine_orbis.sh). The
 * layer's functions are C++ (namespace orbis); they are named here by their
 * symbols. Linked into ntdll.prx only. */
#include <stddef.h>

extern void pw_orbis_thread_counts(unsigned long *created, unsigned long *raised)
    __asm__("_ZN5orbis12threadCountsEPmS0_");
extern void pw_orbis_thread_failures(unsigned long *create_failed, unsigned long *attr_failed)
    __asm__("_ZN5orbis14threadFailuresEPmS0_");
extern size_t pw_orbis_thread_stack_floor(void) __asm__("_ZN5orbis16threadStackFloorEv");

/* Threads created through the layer, how many of them got a larger stack
 * than they asked for, how many creates failed, and the floor in bytes (0
 * before the layer has resolved it). Any pointer may be NULL. */
void pw_wine_thread_stats(unsigned long *created, unsigned long *raised, unsigned long *create_failed,
                          unsigned long long *floor_bytes)
{
    unsigned long attr_failed = 0;

    pw_orbis_thread_counts(created, raised);
    pw_orbis_thread_failures(create_failed, &attr_failed);
    if (floor_bytes) *floor_bytes = pw_orbis_thread_stack_floor();
}
