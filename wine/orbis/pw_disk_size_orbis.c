/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_disk_size_orbis.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* libkernel's (FreeBSD's) fstatfs: fills a struct statfs, 0 or -1 with errno. */
extern int _fstatfs(int fd, void *buf);

static uint64_t u64_at(const unsigned char *p) { uint64_t v; memcpy(&v, p, sizeof(v)); return v; }

static void by_gib(unsigned long long free_gib, unsigned long long total_gib, unsigned long long out[4])
{
    out[0] = 4096;
    out[1] = total_gib << 18;   /* GiB in 4 KiB blocks */
    out[2] = out[3] = free_gib << 18;
}

int pw_orbis_volume_size(int fd, const char *override_gib, unsigned long long out[4], long long diag[4])
{
    union { unsigned char bytes[PW_DISK_SIZE_STATFS_BYTES]; uint64_t align; } buf;
    unsigned long long gib = 0;
    uint64_t bsize, blocks, bfree;
    int64_t bavail;
    uint32_t version;
    int rc;

    memset(&buf, 0, sizeof(buf));
    errno = 0;
    rc = _fstatfs(fd, buf.bytes);
    diag[0] = rc;
    diag[1] = rc ? errno : 0;
    memcpy(&version, buf.bytes, sizeof(version));
    bsize = u64_at(buf.bytes + 16);
    blocks = u64_at(buf.bytes + 32);
    bfree = u64_at(buf.bytes + 40);
    memcpy(&bavail, buf.bytes + 48, sizeof(bavail));
    diag[2] = version;
    diag[3] = bavail;

    if (override_gib && *override_gib)
    {
        char *end;
        gib = strtoull(override_gib, &end, 10);
        if (*end || !gib || gib > (1ull << 20)) gib = 0;
    }
    if (gib)
    {
        by_gib(gib, 2 * gib, out);
        return PW_DISK_SIZE_OVERRIDE;
    }
    if (rc || !blocks || !bsize)
    {
        by_gib(PW_DISK_SIZE_FALLBACK_FREE_GIB, PW_DISK_SIZE_FALLBACK_TOTAL_GIB, out);
        return PW_DISK_SIZE_FALLBACK;
    }
    out[0] = bsize;
    out[1] = blocks;
    out[2] = bfree;
    out[3] = bavail > 0 ? (unsigned long long)bavail : 0;
    return PW_DISK_SIZE_STATFS;
}
