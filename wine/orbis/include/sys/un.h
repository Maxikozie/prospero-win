/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* FreeBSD's sys/un.h (BSD-2-Clause facts, restated): sun_len first, as the
 * kernel lays it out; the SDK's musl header has Linux's. */
#ifndef _ORBIS_WINE_SYS_UN_H
#define _ORBIS_WINE_SYS_UN_H

#include <sys/socket.h>

struct sockaddr_un {
    unsigned char sun_len;        /* sockaddr len including null */
    sa_family_t   sun_family;     /* AF_UNIX */
    char          sun_path[104];  /* path name (gag) */
};

#define SUN_LEN(su) (sizeof(*(su)) - sizeof((su)->sun_path) + strlen((su)->sun_path))

/* Socket options */
#define LOCAL_PEERCRED  1   /* retrieve peer credentials */
#define LOCAL_CREDS     2   /* pass credentials to receiver */
#define LOCAL_CONNWAIT  4   /* connects block until accepted */

#endif /* _ORBIS_WINE_SYS_UN_H */
