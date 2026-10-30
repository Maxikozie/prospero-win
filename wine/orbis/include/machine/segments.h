/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Selector macros for Wine's signal_x86_64.c (FreeBSD branch), in the
 * shape of FreeBSD's sys/amd64/include/segments.h (BSD-2-Clause), with
 * the console's GDT indices.
 *
 * Measured on the PS4 (milestone a probe, 2026-10-30) and on the PS5
 * (prospero-win): a 64-bit thread runs with cs 0x43 and ss 0x3b, and a far
 * jump to 0x33 executes 32-bit code and returns through 0x43. Those are
 * GSEL(8, 3), GSEL(7, 3) and GSEL(6, 3): the console's user descriptors sit
 * two slots above stock FreeBSD 9's (GUCODE32_SEL 3, GUDATA_SEL 4,
 * GUCODE_SEL 5). Only GUCODE32_SEL is read by Wine (cs32_sel for WoW64);
 * the 64-bit selectors it takes from %cs and %ss at run time. */
#ifndef _ORBIS_WINE_MACHINE_SEGMENTS_H
#define _ORBIS_WINE_MACHINE_SEGMENTS_H

#define SEL_UPL 3          /* user priority level */
#define SEL_KPL 0

#define GSEL(s, r) (((s) << 3) | r)   /* a global selector */
#define LSEL(s, r) (((s) << 3) | r | 4)  /* a local selector */

#define GNULL_SEL     0
#define GCODE_SEL     1
#define GDATA_SEL     2
#define GUCODE32_SEL  6    /* measured 0x33 */
#define GUDATA_SEL    7    /* measured 0x3b */
#define GUCODE_SEL    8    /* measured 0x43 */

/* User segment descriptor, for the sysarch(I386_SET_LDT) arguments. The
 * console refuses LDT changes (patch 0500 keeps thread_data->fs at 0), so
 * this is only compiled, never filled. */
struct user_segment_descriptor {
    unsigned long sd_lolimit : 16;  /* segment extent (lsb) */
    unsigned long sd_lobase  : 24;  /* segment base address (lsb) */
    unsigned long sd_type    : 5;   /* segment type */
    unsigned long sd_dpl     : 2;   /* segment descriptor priority level */
    unsigned long sd_p       : 1;   /* segment descriptor present */
    unsigned long sd_hilimit : 4;   /* segment extent (msb) */
    unsigned long sd_xx      : 1;   /* unused */
    unsigned long sd_long    : 1;   /* long mode (cs only) */
    unsigned long sd_def32   : 1;   /* default 32 vs 16 bit size */
    unsigned long sd_gran    : 1;   /* limit granularity (byte/page units) */
    unsigned long sd_hibase  : 8;   /* segment base address (msb) */
} __attribute__((packed));

#endif /* _ORBIS_WINE_MACHINE_SEGMENTS_H */
