/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The write wraps of a Wine PRX on the PS4: with PW_ORBIS_STDOUT_FD and
 * PW_ORBIS_STDERR_FD set, writes to 1 and 2 (plain and vectored, so stdio's
 * too) land on those descriptors; other descriptors and unset variables
 * are untouched. Built with --wrap=write --wrap=writev. */
#define _GNU_SOURCE
#include "../wine/orbis/pw_wine_stdio_orbis.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

static char *read_all(int fd, char *buffer, size_t size)
{
    ssize_t got = read(fd, buffer, size - 1);
    buffer[got > 0 ? got : 0] = 0;
    return buffer;
}

int main(void)
{
    int out[2], err[2], other[2];
    char text[64], env[16];
    struct iovec iov[2] = { { "ab", 2 }, { "cd", 2 } };

    assert(!pipe(out) && !pipe(err) && !pipe(other));

    /* Unset: writes stay where they were. */
    unsetenv("PW_ORBIS_STDOUT_FD");
    unsetenv("PW_ORBIS_STDERR_FD");
    pw_wine_stdio_reset();
    assert(write(other[1], "x", 1) == 1 && !strcmp(read_all(other[0], text, sizeof(text)), "x"));

    snprintf(env, sizeof(env), "%d", out[1]);
    setenv("PW_ORBIS_STDOUT_FD", env, 1);
    snprintf(env, sizeof(env), "%d", err[1]);
    setenv("PW_ORBIS_STDERR_FD", env, 1);
    pw_wine_stdio_reset();

    assert(write(1, "to-out\n", 7) == 7);
    assert(!strcmp(read_all(out[0], text, sizeof(text)), "to-out\n"));
    assert(writev(2, iov, 2) == 4);
    assert(!strcmp(read_all(err[0], text, sizeof(text)), "abcd"));
    /* (The host's libc reaches the kernel without the wrapped symbols, so
     * stdio itself is not checked here; the SDK's musl calls _writev, which
     * the module's __wrap__writev covers.) */
    assert(write(other[1], "y", 1) == 1 && !strcmp(read_all(other[0], text, sizeof(text)), "y"));

    /* A value that is not a usable descriptor leaves the fd alone. */
    setenv("PW_ORBIS_STDOUT_FD", "2", 1);
    pw_wine_stdio_reset();
    assert(write(1, "", 0) == 0);   /* fd 1 itself: the test's stdout */

    printf("pw_wine_stdio_orbis: ok\n");
    return 0;
}
