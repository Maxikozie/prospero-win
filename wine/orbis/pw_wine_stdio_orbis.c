/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Where a Wine PRX's standard output and error go on the PS4.
 *
 * A title cannot put its own descriptor on 1 or 2 (dup2 is refused), so
 * what a module writes there reaches only the kernel log, which the title
 * does not see and the host's klog stream loses under load. The title keeps
 * the writer ends of its forwarding socket pairs and names them in
 * PW_ORBIS_STDOUT_FD and PW_ORBIS_STDERR_FD (patch 1010 gives Wine's
 * standard handles the same descriptors). Linked with --wrap=write
 * --wrap=writev --wrap=_write --wrap=_writev, every write to 1 or 2 inside
 * the module, musl's stdio included (it calls _writev), goes to those
 * descriptors instead. */
#include "pw_wine_stdio_orbis.h"
#include <stdlib.h>
#include <sys/uio.h>
#include <unistd.h>

extern ssize_t __real_write(int fd, const void *data, size_t bytes);
extern ssize_t __real_writev(int fd, const struct iovec *iov, int count);

/* -1 unknown, 0 none, else the descriptor. Read once from the environment,
 * which the title (or the loader shim) sets before any Wine code runs. */
static int redirect[3] = { -1, -1, -1 };

static int target(int fd)
{
    const char *name, *value;
    int chosen;

    if (fd != 1 && fd != 2) return fd;
    chosen = __atomic_load_n(&redirect[fd], __ATOMIC_ACQUIRE);
    if (chosen >= 0) return chosen ? chosen : fd;
    name = fd == 1 ? "PW_ORBIS_STDOUT_FD" : "PW_ORBIS_STDERR_FD";
    value = getenv(name);
    chosen = value ? atoi(value) : 0;
    if (chosen <= 2) chosen = 0;
    __atomic_store_n(&redirect[fd], chosen, __ATOMIC_RELEASE);
    return chosen ? chosen : fd;
}

void pw_wine_stdio_reset(void)
{
    for (int fd = 1; fd <= 2; fd++) __atomic_store_n(&redirect[fd], -1, __ATOMIC_RELEASE);
}

ssize_t __wrap_write(int fd, const void *data, size_t bytes)
{
    return __real_write(target(fd), data, bytes);
}

ssize_t __wrap_writev(int fd, const struct iovec *iov, int count)
{
    return __real_writev(target(fd), iov, count);
}

#ifdef __ORBIS__
/* The SDK's musl stdio calls libkernel's _write and _writev by those names
 * (llvm-nm: __stdio_write.lo references _write and _writev), so a module's
 * fprintf(stderr) goes through these. */
extern ssize_t __real__write(int fd, const void *data, size_t bytes);
extern ssize_t __real__writev(int fd, const struct iovec *iov, int count);

ssize_t __wrap__write(int fd, const void *data, size_t bytes)
{
    return __real__write(target(fd), data, bytes);
}

ssize_t __wrap__writev(int fd, const struct iovec *iov, int count)
{
    return __real__writev(target(fd), iov, count);
}
#endif
