/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* libc gaps of the OpenOrbis SDK that Wine's Unix side hits and
 * wine/ps5/pw_wine_compat_libc.c (written for the PS5 payload SDK) does
 * not cover. Linked into ntdll.prx and wineserver.prx beside it. Every
 * function here is one libkernel exports under another name, or one the
 * kernel refuses; nothing issues a system call itself. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>

/* FreeBSD's thread id, for Wine's get_unix_tid fallback and the server's
 * thread registry: libkernel exports pthread_getthreadid_np (declared by
 * orbis-compat's pthread_np.h as FreeBSD does). */
extern int pthread_getthreadid_np(void);

int thr_self(long *id)
{
    *id = pthread_getthreadid_np();
    return 0;
}

/* The SDK's libc.a has a ppoll that returns ENOSYS; libkernel exports
 * poll. The signal mask argument is applied around the wait as FreeBSD's
 * ppoll does, which is all Wine's server loop (poll with a timeout) needs. */
extern int pthread_sigmask(int how, const sigset_t *set, sigset_t *old);

int ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout, const sigset_t *mask)
{
    sigset_t previous;
    int ms = -1, rc, saved;

    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000L) {
            errno = EINVAL;
            return -1;
        }
        ms = (int)(timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999L) / 1000000L);
    }
    if (mask) pthread_sigmask(SIG_SETMASK, mask, &previous);
    rc = poll(fds, count, ms);
    saved = errno;
    if (mask) pthread_sigmask(SIG_SETMASK, &previous, NULL);
    errno = saved;
    return rc;
}

/* ---- stat on the PS4 ------------------------------------------------------
 *
 * The SDK's struct stat is musl's and the kernel's is FreeBSD 9's;
 * orbis-compat translates the kernel bytes (dev u32 at 0, ino u32 at 4,
 * mode u16 at 8, ... size i64 at 0x48, times at 0x18/0x28/0x38). Measured on
 * the console: sceKernelStat (the path call) fills the buffer but leaves
 * dev and ino 0, while _fstat on an open descriptor returns them. Wine keys
 * file identity on dev/ino (the server's fd table, directory identity
 * checks), so these four replace orbis-compat's: a path stat whose kernel
 * answer carries no identity is repeated through open + _fstat. The raw
 * kernel bytes of the first path stat are written to stderr once, so the
 * layout is on record. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

extern int sceKernelStat(const char *path, void *raw);
extern int _fstat(int fd, void *raw);
extern int __real_open(const char *path, int flags, ...);
#define PW_STAT_RAW 256

static uint64_t raw_u64(const unsigned char *raw, size_t at) { uint64_t v; memcpy(&v, raw + at, 8); return v; }
static uint32_t raw_u32(const unsigned char *raw, size_t at) { uint32_t v; memcpy(&v, raw + at, 4); return v; }
static uint16_t raw_u16(const unsigned char *raw, size_t at) { uint16_t v; memcpy(&v, raw + at, 2); return v; }

static void stat_translate(const unsigned char *raw, struct stat *out)
{
    memset(out, 0, sizeof(*out));
    out->st_dev = raw_u32(raw, 0x00);
    out->st_ino = raw_u32(raw, 0x04);
    out->st_mode = raw_u16(raw, 0x08);
    out->st_nlink = raw_u16(raw, 0x0a);
    out->st_uid = raw_u32(raw, 0x0c);
    out->st_gid = raw_u32(raw, 0x10);
    out->st_rdev = raw_u32(raw, 0x14);
    out->st_atim.tv_sec = (time_t)raw_u64(raw, 0x18); out->st_atim.tv_nsec = (long)raw_u64(raw, 0x20);
    out->st_mtim.tv_sec = (time_t)raw_u64(raw, 0x28); out->st_mtim.tv_nsec = (long)raw_u64(raw, 0x30);
    out->st_ctim.tv_sec = (time_t)raw_u64(raw, 0x38); out->st_ctim.tv_nsec = (long)raw_u64(raw, 0x40);
    out->st_size = (off_t)raw_u64(raw, 0x48);
    out->st_blocks = (blkcnt_t)raw_u64(raw, 0x50);
    out->st_blksize = (blksize_t)raw_u32(raw, 0x58);
}

static int sce_errno(int rc)
{
    return ((unsigned)rc >> 16) == 0x8002 ? (rc & 0xffff) : EIO;
}

int fstat(int fd, struct stat *out)
{
    unsigned char raw[PW_STAT_RAW];
    int rc;
    if (!out) { errno = EFAULT; return -1; }
    memset(raw, 0, sizeof(raw));
    if ((rc = _fstat(fd, raw)) != 0) return rc;
    stat_translate(raw, out);
    return 0;
}

int stat(const char *path, struct stat *out)
{
    static int dumped;
    unsigned char raw[PW_STAT_RAW];
    int rc;

    if (!path || !out) { errno = EFAULT; return -1; }
    memset(raw, 0, sizeof(raw));
    if ((rc = sceKernelStat(path, raw)) != 0) { errno = sce_errno(rc); return -1; }
    if (!__atomic_exchange_n(&dumped, 1, __ATOMIC_RELAXED)) {
        char hex[2 * 120 + 1];
        for (int i = 0; i < 120; i++) snprintf(hex + 2 * i, 3, "%02x", raw[i]);
        fprintf(stderr, "wine-orbis: sceKernelStat(%s) raw=%s\n", path, hex);
    }
    stat_translate(raw, out);
    if (!out->st_dev && !out->st_ino) {
        /* No identity from the path call: the descriptor's answer has it. */
        int fd = __real_open(path, O_RDONLY);
        if (fd >= 0) {
            struct stat by_fd;
            if (fstat(fd, &by_fd) == 0) { out->st_dev = by_fd.st_dev; out->st_ino = by_fd.st_ino; }
            close(fd);
        }
    }
    return 0;
}

/* No symlinks exist on the file systems a title reads (the virtual ones
 * live in wine/ps5/pw_wine_cwd.c, above this). */
int lstat(const char *path, struct stat *out) { return stat(path, out); }

int fstatat(int fd, const char *path, struct stat *out, int flags)
{
    (void)flags;
    if (!path || !out) { errno = EFAULT; return -1; }
    if (path[0] == '/' || fd == AT_FDCWD) return stat(path, out);
    errno = ENOSYS;
    return -1;
}

/* ---- dup on the PS4 ---------------------------------------------------------
 *
 * Measured in milestone c run 9: dup() returns EPERM (as dup2 onto 1 and 2
 * does), and Wine's server duplicates the descriptor of every mapping and
 * every duplicated handle. Linked with --wrap=dup: F_DUPFD is tried, then,
 * for a descriptor the cwd shim knows the path of, the file is opened again
 * with the same flags and offset. Anonymous descriptors (sockets, pipes,
 * shared memory) have no path; those stay EPERM, which the first run with
 * this shim reports. */
#include <sys/types.h>

extern int __real_dup(int fd);
extern int pw_cwd_fd_path(int fd, char *path, size_t size);

int __wrap_dup(int fd)
{
    static int logged;
    int d = __real_dup(fd), saved = errno, flags;
    char path[1024];

    if (d >= 0) return d;
    if ((d = fcntl(fd, F_DUPFD, 3)) >= 0) return d;
    if (!pw_cwd_fd_path(fd, path, sizeof(path))) {
        off_t at = lseek(fd, 0, SEEK_CUR);
        flags = fcntl(fd, F_GETFL);
        if (flags < 0) flags = O_RDONLY;
        d = __real_open(path, flags & ~(O_CREAT | O_EXCL | O_TRUNC));
        if (d >= 0) {
            if (at >= 0) lseek(d, at, SEEK_SET);
            if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
                fprintf(stderr, "wine-orbis: dup refused (errno %d); files are reopened by path\n", saved);
            return d;
        }
    }
    errno = saved;
    return -1;
}
