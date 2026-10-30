/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* orbis-compat's pthread_np.h plus pthread_getthreadid_np, which libkernel
 * exports (FreeBSD's thread id, what Wine's get_unix_tid returns). */
#ifndef _ORBIS_WINE_PTHREAD_NP_H
#define _ORBIS_WINE_PTHREAD_NP_H

#include_next <pthread_np.h>

#ifdef __cplusplus
extern "C" {
#endif
int pthread_getthreadid_np(void);
#ifdef __cplusplus
}
#endif

#endif /* _ORBIS_WINE_PTHREAD_NP_H */
