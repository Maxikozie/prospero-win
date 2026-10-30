/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* orbis-compat's sys/ioctl.h (FreeBSD FION* over the SDK's) plus the
 * FreeBSD ioctls Wine's Unix side uses (BSD-2-Clause facts, restated from
 * FreeBSD 9's sys/ttycom.h, sys/sockio.h and sys/filio.h), encoded with
 * orbis-compat's sys/ioccom.h, which has FreeBSD's _IO* layout rather than
 * Linux's. */
#ifndef _ORBIS_WINE_SYS_IOCTL_H
#define _ORBIS_WINE_SYS_IOCTL_H

#include_next <sys/ioctl.h>
#include <sys/ioccom.h>

/* struct winsize is the SDK's (the same four shorts); the request numbers
 * are FreeBSD's, not the Linux ones the SDK may carry. */
#undef TIOCGWINSZ
#undef TIOCSWINSZ
#define TIOCGWINSZ _IOR('t', 104, struct winsize)   /* get window size */
#define TIOCSWINSZ _IOW('t', 103, struct winsize)   /* set window size */

#ifndef SIOCATMARK
#define SIOCATMARK _IOR('s', 7, int)                /* at oob mark? */
#endif

#ifndef FIODTYPE
#define FIODTYPE _IOR('f', 122, int)                /* get d_flags type part */
#define D_TAPE 0x0001
#define D_DISK 0x0002
#define D_TTY  0x0004
#define D_MEM  0x0008
#endif

#endif /* _ORBIS_WINE_SYS_IOCTL_H */
