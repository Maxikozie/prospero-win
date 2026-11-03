/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* wine/orbis/pw_disk_size_orbis.c with a scripted _fstatfs: the fill from a
 * FreeBSD struct statfs, the fallback when it fails or reports no blocks,
 * the WINE_PS5_DISK_FREE_GIB override, and what Psychonauts.exe computes
 * from GetDiskFreeSpaceA (sectors * bytes * free clusters in 32 bits, with
 * ntdll's 512-byte sectors, 8 a cluster, as get_full_size_info sets them). */
#include "../wine/orbis/pw_disk_size_orbis.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int script_rc, script_errno;
static uint32_t script_version;
static uint64_t script_bsize, script_blocks, script_bfree;
static int64_t script_bavail;
static size_t touched;   /* the largest offset written + 1: must fit the caller's buffer */

int _fstatfs(int fd, void *buf)
{
    unsigned char *p = buf;
    (void)fd;
    /* FreeBSD 12 writes the whole struct statfs (0x928 bytes). */
    memset(p, 0xa5, 0x928);
    touched = 0x928;
    memcpy(p, &script_version, 4);
    memcpy(p + 16, &script_bsize, 8);
    memcpy(p + 32, &script_blocks, 8);
    memcpy(p + 40, &script_bfree, 8);
    memcpy(p + 48, &script_bavail, 8);
    if (script_rc) { errno = script_errno; return -1; }
    return 0;
}

/* GetDiskFreeSpaceA's DWORDs (kernelbase on NT: cluster = 8 * 512 unless bsize is 2048) multiplied in 32 bits. */
static uint32_t game_free_bytes(const unsigned long long out[4])
{
    uint32_t sectors = out[0] == 2048 ? 1 : 8, bytes = out[0] == 2048 ? 2048 : 512;
    uint32_t clusters = (uint32_t)(out[0] * out[3] / (sectors * bytes));
    return sectors * bytes * clusters;
}

int main(void)
{
    unsigned long long out[4];
    long long diag[4];
    int source;

    assert(PW_DISK_SIZE_STATFS_BYTES >= 0x928);

    /* A real answer: 4 KiB blocks, 300 GiB, 160 GiB free, 150 GiB available. */
    script_rc = 0; script_version = 0x20140518; script_bsize = 4096;
    script_blocks = 300ull << 18; script_bfree = 160ull << 18; script_bavail = 150ll << 18;
    source = pw_orbis_volume_size(3, NULL, out, diag);
    assert(source == PW_DISK_SIZE_STATFS);
    assert(out[0] == 4096 && out[1] == 300ull << 18 && out[2] == 160ull << 18 && out[3] == 150ull << 18);
    assert(diag[0] == 0 && diag[1] == 0 && diag[2] == 0x20140518 && diag[3] == 150ll << 18);
    assert(touched <= PW_DISK_SIZE_STATFS_BYTES);

    /* Negative f_bavail (root's reserve exceeded) is none available. */
    script_bavail = -5;
    source = pw_orbis_volume_size(3, "", out, diag);
    assert(source == PW_DISK_SIZE_STATFS && out[3] == 0);

    /* _fstatfs fails: the fallback, and its errno recorded. */
    script_rc = 1; script_errno = ENOSYS; script_bavail = 0;
    source = pw_orbis_volume_size(3, NULL, out, diag);
    assert(source == PW_DISK_SIZE_FALLBACK && diag[0] == -1 && diag[1] == ENOSYS);
    assert(out[0] == 4096 && out[1] == 130ull << 18 && out[2] == 65ull << 18 && out[3] == 65ull << 18);
    assert(game_free_bytes(out) == 1u << 30);   /* 65 GiB reads as 1 GiB in 32 bits */

    /* Succeeds with no blocks (or no block size): the fallback. */
    script_rc = 0; script_blocks = 0; script_bsize = 4096;
    assert(pw_orbis_volume_size(3, NULL, out, diag) == PW_DISK_SIZE_FALLBACK);
    script_blocks = 10; script_bsize = 0;
    assert(pw_orbis_volume_size(3, NULL, out, diag) == PW_DISK_SIZE_FALLBACK);

    /* Why not 64 GiB: it reads as 0 in 32 bits. */
    {
        unsigned long long sixty_four[4] = { 4096, 128ull << 18, 64ull << 18, 64ull << 18 };
        assert(game_free_bytes(sixty_four) == 0);
    }

    /* The override wins over both a real answer and the fallback; junk is ignored. */
    script_rc = 0; script_bsize = 4096; script_blocks = 300ull << 18; script_bfree = script_bavail = 160ll << 18;
    source = pw_orbis_volume_size(3, "33", out, diag);
    assert(source == PW_DISK_SIZE_OVERRIDE && out[2] == 33ull << 18 && out[3] == 33ull << 18 && out[1] == 66ull << 18);
    script_rc = 1;
    assert(pw_orbis_volume_size(3, "33", out, diag) == PW_DISK_SIZE_OVERRIDE);
    assert(pw_orbis_volume_size(3, "12x", out, diag) == PW_DISK_SIZE_FALLBACK);
    assert(pw_orbis_volume_size(3, "0", out, diag) == PW_DISK_SIZE_FALLBACK);

    printf("test_pw_disk_size_orbis: the fill from FreeBSD's struct statfs (0x928 bytes), the 65/130 GiB fallback "
           "on failure or no blocks (1 GiB in the game's 32-bit product; 64 GiB would be 0), the override\n");
    return 0;
}
