/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The SDK's sys/resource.h plus FreeBSD's CPU state indices, which Wine's
 * system.c reads from the kern.cp_time sysctl under __FreeBSD__
 * (BSD-2-Clause facts from FreeBSD 9's sys/resource.h). */
#ifndef _ORBIS_WINE_SYS_RESOURCE_H
#define _ORBIS_WINE_SYS_RESOURCE_H

#include_next <sys/resource.h>

#ifndef CPUSTATES
#define CP_USER  0
#define CP_NICE  1
#define CP_SYS   2
#define CP_INTR  3
#define CP_IDLE  4
#define CPUSTATES 5
#endif

#endif /* _ORBIS_WINE_SYS_RESOURCE_H */
