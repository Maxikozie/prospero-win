/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* FreeBSD's sys/mount.h for Wine's Unix side on the PS4: struct statfs with
 * the mount names Wine reads (f_mntonname, f_mntfromname), the MNT_ flags
 * and getmntinfo (BSD-2-Clause facts, restated from FreeBSD 9). The SDK's
 * musl sys/statfs.h and sys/vfs.h resolve to this file too, so one layout
 * is seen everywhere. A title sees no mount table: wine/ps5's getmntinfo
 * returns 0 entries and statfs/fstatfs fail with ENOSYS. */
#ifndef _ORBIS_WINE_SYS_MOUNT_H
#define _ORBIS_WINE_SYS_MOUNT_H

#include <sys/types.h>
#include <stdint.h>

#define MFSNAMELEN 16
#define MNAMELEN   88

typedef struct fsid { int32_t val[2]; } fsid_t;

struct statfs {
    uint32_t f_version;             /* structure version number */
    uint32_t f_type;                /* type of filesystem */
    uint64_t f_flags;               /* copy of mount exported flags */
    uint64_t f_bsize;               /* filesystem fragment size */
    uint64_t f_iosize;              /* optimal transfer block size */
    uint64_t f_blocks;              /* total data blocks in filesystem */
    uint64_t f_bfree;               /* free blocks in filesystem */
    int64_t  f_bavail;              /* free blocks avail to non-superuser */
    uint64_t f_files;               /* total file nodes in filesystem */
    int64_t  f_ffree;               /* free nodes avail to non-superuser */
    uint64_t f_syncwrites;          /* count of sync writes since mount */
    uint64_t f_asyncwrites;         /* count of async writes since mount */
    uint64_t f_syncreads;           /* count of sync reads since mount */
    uint64_t f_asyncreads;          /* count of async reads since mount */
    uint64_t f_spare[10];           /* unused spare */
    uint32_t f_namemax;             /* maximum filename length */
    uid_t    f_owner;               /* user that mounted the filesystem */
    fsid_t   f_fsid;                /* filesystem id */
    char     f_charspare[80];       /* spare string space */
    char     f_fstypename[MFSNAMELEN]; /* filesystem type name */
    char     f_mntfromname[MNAMELEN];  /* mounted filesystem */
    char     f_mntonname[MNAMELEN];    /* directory on which mounted */
};

#define MNT_RDONLY      0x0000000000000001ULL
#define MNT_SYNCHRONOUS 0x0000000000000002ULL
#define MNT_NOEXEC      0x0000000000000004ULL
#define MNT_NOSUID      0x0000000000000008ULL
#define MNT_LOCAL       0x0000000000001000ULL
#define MNT_QUOTA       0x0000000000002000ULL
#define MNT_ROOTFS      0x0000000000004000ULL

#define MNT_WAIT   1
#define MNT_NOWAIT 2

#ifdef __cplusplus
extern "C" {
#endif
int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);
int getmntinfo(struct statfs **mntbufp, int mode);
#ifdef __cplusplus
}
#endif

#endif /* _ORBIS_WINE_SYS_MOUNT_H */
