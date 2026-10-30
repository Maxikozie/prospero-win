/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* FreeBSD's sys/thr.h (BSD-2-Clause) for Wine's Unix side: the SDK does
 * not ship it, and without it Wine's server signals threads with kill(),
 * which reaches the process, not the thread. libkernel exports none of
 * these (checked with llvm-nm on the SDK's stubs): thr_kill2 comes from
 * wine/ps5/pw_wine_threads_libc.c (a tid registry over pthread_kill),
 * thr_set_name from wine/ps5/pw_wine_compat_libc.c and thr_self from
 * wine/orbis/pw_wine_compat_orbis.c (pthread_getthreadid_np, which
 * libkernel does export). */
#ifndef _ORBIS_WINE_SYS_THR_H
#define _ORBIS_WINE_SYS_THR_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif
int thr_self(long *id);
int thr_kill(long id, int sig);
int thr_kill2(pid_t pid, long id, int sig);
int thr_set_name(long id, const char *name);   /* a no-op in wine/ps5/pw_wine_compat_libc.c */
#ifdef __cplusplus
}
#endif

#endif /* _ORBIS_WINE_SYS_THR_H */
