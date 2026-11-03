/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_DISK_SIZE_ORBIS_H
#define PW_DISK_SIZE_ORBIS_H
/*
 * A volume's size for ntdll's FileFsSizeInformation on the PS4, linked into
 * ntdll.prx (patch 1150 calls it from get_full_size_info).
 *
 * libc.a's fstatvfs calls libkernel's _fstatfs into a frame of its own and
 * returns 0 or -1 without filling the caller's struct statvfs (OpenOrbis
 * statvfs.lo), so on success Wine read uninitialised stack. Here _fstatfs
 * fills a FreeBSD struct statfs directly, into a buffer at least as large as
 * FreeBSD 12's (0x928 bytes; its leading fields are FreeBSD 9's too):
 *   +0 f_version (u32), +16 f_bsize, +32 f_blocks, +40 f_bfree (u64),
 *   +48 f_bavail (i64).
 * When _fstatfs fails or reports no blocks: 65 GiB free of 130 GiB
 * (Psychonauts.exe multiplies GetDiskFreeSpaceA's three DWORDs in 32 bits,
 * 0x553d35: 64 GiB would read as 0, 65 GiB reads as 1 GiB).
 * WINE_PS5_DISK_FREE_GIB (a whole number of GiB) overrides both: that much
 * free, twice that in total.
 */

enum { PW_DISK_SIZE_STATFS = 0, PW_DISK_SIZE_FALLBACK = 1, PW_DISK_SIZE_OVERRIDE = 2 };
enum { PW_DISK_SIZE_STATFS_BYTES = 0x1000 };   /* >= FreeBSD 12's struct statfs (0x928) */
enum { PW_DISK_SIZE_FALLBACK_FREE_GIB = 65, PW_DISK_SIZE_FALLBACK_TOTAL_GIB = 130 };

/* out[0..3] = bsize, blocks, bfree, bavail (blocks of bsize bytes);
 * diag[0..3] = _fstatfs's rc, its errno (0 on success), f_version, f_bavail as
 * reported (before any fallback, low 32 bits). Returns PW_DISK_SIZE_*. */
int pw_orbis_volume_size(int fd, const char *override_gib, unsigned long long out[4], long long diag[4]);

#endif
