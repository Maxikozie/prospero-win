/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The counters the backend's Unix side exports as pw_wow_stats, for a title
 * that loads the module itself (the PS4 port reads them through
 * sceKernelDlsym at the end of a run). Plain C: no Wine types. New counters
 * go last, so the existing ones keep their numbers. */
#ifndef WOWPROSPERO_WOW_STATS_H
#define WOWPROSPERO_WOW_STATS_H

#include <stdint.h>

enum pw_wow_stat
{
    PW_WOW_STAT_THREADS,           /* threads that got a translator */
    PW_WOW_STAT_FAULTS_GUEST,      /* faults run() handed back to the guest (its EXCEPTION_ACCESS_VIOLATION) */
    PW_WOW_STAT_FAULTS_MARKED,     /* host faults at marked accesses the hooks resumed at their refused-access path */
    PW_WOW_STAT_FAULTS_UNRESOLVED, /* faults in translated code nobody resolved: an unmarked access Wine could
                                    * not resolve, or an instruction the host refused (SIGILL) */
    PW_WOW_STAT_FAULTS_HOST,       /* access violations Wine could not resolve outside translated code; Wine
                                    * raises them as its own exceptions (a __TRY often catches them) */
    PW_WOW_STAT_FAULTS_SMC,        /* writes to code pages write-protected for their translations */
    PW_WOW_STAT_FAULTS_CALL_STACK, /* calls that ran out of the call stack and started it again */
    PW_WOW_STAT_DBT_ERRORS,        /* run() stops for an instruction it could not translate or run, or an error */
    PW_WOW_STAT_COUNT
};

/* unsigned int pw_wow_stats(uint64_t *out, unsigned int count): fills
 * out[0 .. min(count, PW_WOW_STAT_COUNT)) and returns PW_WOW_STAT_COUNT.
 * Counts since the module was loaded; the hooks count only where they run
 * (FAULTS_UNRESOLVED and FAULTS_HOST: the consoles' Wine hooks). */
typedef unsigned int (*pw_wow_stats_fn)( uint64_t *out, unsigned int count );

#endif
