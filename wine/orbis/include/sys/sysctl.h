/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* orbis-compat's sys/sysctl.h (CTL_HW, HW_NCPU, KERN_PROC, ...) plus the
 * names Wine's system.c reads under __FreeBSD__ (BSD-2-Clause facts from
 * FreeBSD 9's sys/sysctl.h and sys/time.h). Whether the console answers
 * these sysctls is a run-time question; Wine falls back when it does not. */
#ifndef _ORBIS_WINE_SYS_SYSCTL_H
#define _ORBIS_WINE_SYS_SYSCTL_H

#include_next <sys/sysctl.h>
#include <stddef.h>

#ifndef KERN_CLOCKRATE
#define KERN_CLOCKRATE 12
#endif
#ifndef KERN_PROC_PID
#define KERN_PROC_PID 1
#endif
#ifndef HW_PAGESIZE
#define HW_PAGESIZE 7
#endif
#ifndef HW_REALMEM
#define HW_REALMEM 18
#endif

struct clockinfo {
    int hz;         /* clock frequency */
    int tick;       /* micro-seconds per hz tick */
    int spare;
    int stathz;     /* statistics clock frequency */
    int profhz;     /* profiling clock frequency */
};

#ifdef __cplusplus
extern "C" {
#endif
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, const void *newp, size_t newlen);
#ifdef __cplusplus
}
#endif

#endif /* _ORBIS_WINE_SYS_SYSCTL_H */
