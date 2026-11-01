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

/* Wine names a thread with thr_set_name when the program does (SetThreadDescription:
 * DXVK's dxvk-cs, dxvk-submit, ...; ntdll/unix/thread.c's FreeBSD branch). libkernel
 * renames only through a thread handle, so the calling thread's name is set and
 * another thread's is refused; the kernel then reports the name as the thread's
 * ki_tdname, which the PS4 title's CPU census reads (milestone g). */
extern void *scePthreadSelf(void);
extern int scePthreadRename(void *thread, const char *name);

int thr_set_name(long id, const char *name)
{
    if (id != (long)pthread_getthreadid_np()) {
        errno = ESRCH;
        return -1;
    }
    return scePthreadRename(scePthreadSelf(), name) ? (errno = EINVAL, -1) : 0;
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
    unsigned char raw[PW_STAT_RAW];
    int rc;

    if (!path || !out) { errno = EFAULT; return -1; }
    memset(raw, 0, sizeof(raw));
    if ((rc = sceKernelStat(path, raw)) != 0) { errno = sce_errno(rc); return -1; }
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
#include "pw_wine_dup_orbis.h"
extern int pw_cwd_fd_path(int fd, char *path, size_t size);

int __wrap_dup(int fd)
{
    static int logged;
    int d = __real_dup(fd), saved = errno, flags;
    char path[1024];

    if (d >= 0) return d;
    if ((d = fcntl(fd, F_DUPFD, 3)) >= 0) return d;
    /* A real duplicate for every kind of descriptor: passed to ourselves
     * with SCM_RIGHTS (pw_wine_dup_orbis.c). */
    if ((d = pw_orbis_dup_by_passing(fd)) >= 0) {
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
            fprintf(stderr, "wine-orbis: dup refused (errno %d); descriptors are duplicated by passing them\n", saved);
        return d;
    }
    /* Last resort, files only: reopened by path (own offset and flags). */
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

/* ---- ENOSYS stubs of the SDK's libc that Wine calls unchecked ------------
 *
 * tools/orbis_libc_stubs.py lists the functions of libc.a that only set
 * ENOSYS. Wine's ntdll reads uname() for its version string without looking
 * at the result, so an unfilled utsname would be read as text (umask, the
 * other such call, is wine/ps5/pw_wine_compat_libc.c's). A title has no
 * kernel name query: fixed answers. */
#include <sys/utsname.h>

int uname(struct utsname *name)
{
    memset(name, 0, sizeof(*name));
    snprintf(name->sysname, sizeof(name->sysname), "FreeBSD");
    snprintf(name->nodename, sizeof(name->nodename), "ps4");
    snprintf(name->release, sizeof(name->release), "9.0-ORBIS");
    snprintf(name->version, sizeof(name->version), "Orbis OS");
    snprintf(name->machine, sizeof(name->machine), "amd64");
    return 0;
}

/* ---- O_NONBLOCK on the PS4 -------------------------------------------------
 *
 * Measured (console run 12): fcntl(F_SETFL, O_NONBLOCK) on the end of a
 * socket pair that arrived through SCM_RIGHTS fails with EACCES, which cost
 * the in-process server its first client (init_thread gave up without a
 * reply). Wine sets O_NONBLOCK through fcntl everywhere (reply pipes, the
 * wake pipe, sockets), so the call is wrapped (--wrap=fcntl): when the kernel
 * refuses F_SETFL, the same bit goes through FIONBIO, FreeBSD's other door to
 * it, by libkernel's ioctl under its FreeBSD prototype (the SDK's ioctl takes
 * an int, which would sign-extend the request). Each outcome is reported
 * once; nothing else about fcntl changes. */
#include <stdarg.h>

#define ORBIS_FIONBIO 0x8004667eUL   /* FreeBSD _IOW('f', 126, int) */

extern int __real_fcntl(int fd, int cmd, ...);
extern int _ioctl(int fd, unsigned long request, ...);

int __wrap_fcntl(int fd, int cmd, ...)
{
    static int logged[2];
    va_list ap;
    long arg;
    int rc, saved, on;

    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);
    rc = __real_fcntl(fd, cmd, arg);
    if (rc != -1 || cmd != F_SETFL) return rc;
    saved = errno;
    on = (arg & O_NONBLOCK) != 0;
    if (_ioctl(fd, ORBIS_FIONBIO, &on) == 0) {
        if (!__atomic_exchange_n(&logged[0], 1, __ATOMIC_RELAXED))
            fprintf(stderr, "wine-orbis: fcntl(F_SETFL, %#lx) on fd %d refused (errno %d); FIONBIO used instead\n",
                    arg, fd, saved);
        return 0;
    }
    if (!__atomic_exchange_n(&logged[1], 1, __ATOMIC_RELAXED))
        fprintf(stderr, "wine-orbis: fcntl(F_SETFL, %#lx) on fd %d refused (errno %d) and FIONBIO too (errno %d)\n",
                arg, fd, saved, errno);
    errno = saved;
    return -1;
}

/* ---- getauxval ---------------------------------------------------------------
 *
 * Measured (console run 13): SIGSEGV at address 0 in musl's getauxval. The
 * SDK's libc.a reads the aux vector its start-up code saved, and a PRX has
 * no start-up code (the loader runs neither constructors nor crt), so the
 * pointer is NULL. A title has no aux vector to give anyway; Wine's callers
 * (get_image_address, the FSGSBASE check) take 0 as "unknown". */
unsigned long getauxval(unsigned long type)
{
    (void)type;
    errno = ENOENT;
    return 0;
}
