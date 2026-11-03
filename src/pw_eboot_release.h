/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_EBOOT_RELEASE_H
#define PW_EBOOT_RELEASE_H
#include "../wine/ps5/pw_wine_prx.h"
#include <stddef.h>
#include <stdint.h>

/* Releasing the title's eboot on Orbis. The title runs from a high app PRX
 * that a small stub eboot loaded; the stub's segments sit at 0x400000, where
 * Windows programs without relocations want their image. Unmapping every
 * segment of module 0 just before Wine starts leaves that range to Wine's
 * reserve_area. The range is read from module 0's own module info, never a
 * constant. Everything here is pure and host-testable: the console calls come
 * in through PwEbootReleaseOps.
 *
 * From: plastikman/orbis-win src/reloc-wine-app/app_entry.c @ 2610706
 * (LGPL-2.1-or-later): the release sequence (read module 0's segments,
 * sceKernelMunmap each, stop at the first failure). Rewritten here on
 * pw_prx_parse_module_info, with the ranges page-rounded and merged, a 4 GiB
 * limit and a report of what module 0's info lists afterwards. */

#define PW_EBOOT_PAGE ((uintptr_t)0x4000)
#define PW_EBOOT_LIMIT ((uintptr_t)1 << 32)
/* Segments from here up are kept: the top of Wine's 32-bit reservations (mmap_init reserves
 * 0x10000-0x7fff0000). The stub's read-write segment lives there (tools/pw_stub_orbis.ld), so module
 * 0's data stays mapped after the release: libkernel's sceKernelGetModuleList reads it (ps4-wine LOG,
 * e at 05:33 on 2026-11-03: a fault when nothing backed it). */
#define PW_EBOOT_KEEP_FROM ((uintptr_t)0x7fff0000)

enum {
    PW_EBOOT_OK = 0,
    PW_EBOOT_ERR_ARGUMENT = -1,
    PW_EBOOT_ERR_MODULE_INFO = -2, /* module info unreadable or malformed */
    PW_EBOOT_ERR_RANGE = -3,       /* a segment of size 0, address 0, reaching past 4 GiB, across
                                      PW_EBOOT_KEEP_FROM, or none below it */
    PW_EBOOT_ERR_UNMAP = -4,       /* an unmap returned nonzero; its rc is in unmap_rc */
};

/* What module 0's info says after the release. */
enum { PW_EBOOT_AFTER_NONE = 0, PW_EBOOT_AFTER_LISTED, PW_EBOOT_AFTER_GONE, PW_EBOOT_AFTER_ERR };

typedef struct PwEbootRange { uintptr_t base, end; } PwEbootRange;

typedef struct PwEbootReleaseOps {
    /* Fills a PW_PRX_MODULE_INFO_BYTES record for module 0 (the size word is
     * already set); returns 0 or the console's error. */
    int (*eboot_info)(void *context, void *info);
    int (*unmap)(void *context, void *address, size_t bytes);
    void *context;
} PwEbootReleaseOps;

typedef struct PwEbootRelease {
    int status;                       /* PW_EBOOT_OK or PW_EBOOT_ERR_* */
    int info_rc;                      /* the eboot_info rc before the release */
    uint32_t segment_count;
    PwPrxSegment segments[PW_PRX_MAX_SEGMENTS];
    uint32_t kept;                    /* segments at or above PW_EBOOT_KEEP_FROM, left mapped */
    uint32_t range_count;             /* merged, page-rounded ranges to unmap */
    PwEbootRange ranges[PW_PRX_MAX_SEGMENTS];
    uintptr_t low_base, low_end;      /* the lowest page and the end of the highest */
    uint32_t unmapped;                /* ranges unmapped successfully, in order */
    int unmap_rc;                     /* the first nonzero unmap rc, else 0 */
    int after;                        /* PW_EBOOT_AFTER_* */
    int after_rc;                     /* the eboot_info rc after the release */
    uint32_t after_count;             /* segments module 0's info still lists */
} PwEbootRelease;

/* Page-rounds every segment below PW_EBOOT_KEEP_FROM (those above it are counted in kept and left
 * alone), merges overlapping or touching ranges (two segments may share a 16 KiB page) and sorts
 * them by address. */
int pw_eboot_release_plan(const PwPrxSegment *segments, uint32_t count, PwEbootRelease *out);
/* Reads module 0, plans, unmaps each range, stops at the first failure, then
 * re-reads module 0 to report what it still lists. */
int pw_eboot_release(const PwEbootReleaseOps *ops, PwEbootRelease *out);
const char *pw_eboot_release_after_name(int after);

/* The title PRX's copy of the argument vector the stub eboot hands over, so
 * nothing the title keeps points into memory it did not allocate itself.
 * Copies at most max_args - 1 arguments and their strings into store (NULs
 * included), terminates out[] with NULL and returns the number copied; an
 * argument that does not fit ends the copy there. */
int pw_eboot_copy_argv(int argc, char *const *argv, char *store, size_t store_size, char **out, int max_args);

#endif
