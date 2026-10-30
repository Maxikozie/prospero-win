/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* sysarch() operations in the shape of FreeBSD's
 * sys/amd64/include/sysarch.h (BSD-2-Clause). libkernel exports sysarch();
 * the OpenOrbis SDK declares neither it nor the operation numbers.
 * AMD64_{GET,SET}_{FS,GS}BASE (128-131) are measured working on the PS4
 * (milestone a probe: %gs loads read through a base set with 131, 129
 * restores FS); the LDT operations are FreeBSD's numbers, refused by the
 * console (patch 0500). */
#ifndef _ORBIS_WINE_MACHINE_SYSARCH_H
#define _ORBIS_WINE_MACHINE_SYSARCH_H

#include <machine/segments.h>

#define I386_GET_LDT     0
#define I386_SET_LDT     1
#define I386_GET_IOPERM  3
#define I386_SET_IOPERM  4
#define I386_GET_FSBASE  7
#define I386_SET_FSBASE  8
#define I386_GET_GSBASE  9
#define I386_SET_GSBASE 10

#define AMD64_GET_FSBASE 128
#define AMD64_SET_FSBASE 129
#define AMD64_GET_GSBASE 130
#define AMD64_SET_GSBASE 131

struct i386_ldt_args {
    unsigned int start;
    struct user_segment_descriptor *descs;
    unsigned int num;
};

#ifdef __cplusplus
extern "C" {
#endif
int sysarch(int, void *);
#ifdef __cplusplus
}
#endif

#endif /* _ORBIS_WINE_MACHINE_SYSARCH_H */
