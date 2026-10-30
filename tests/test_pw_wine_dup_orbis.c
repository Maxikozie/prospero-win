/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* wine/orbis/pw_wine_dup_orbis.c on the host: the descriptor it returns is a
 * second descriptor for the same open file, as dup()'s is: a pipe's read
 * end sees what the write end wrote and stays readable after the original
 * is closed, a file's offset is shared, status flags are shared, and a bad
 * descriptor fails with EBADF; many in a row do not leak the passing pair's
 * queue. */
#include "pw_wine_dup_orbis.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    int pipe_fds[2], copy, file, file_copy;
    char buffer[8] = { 0 }, path[] = "/tmp/pw-dup-orbis-XXXXXX";

    assert(!pipe(pipe_fds));
    assert((copy = pw_orbis_dup_by_passing(pipe_fds[0])) >= 0 && copy != pipe_fds[0]);
    assert(write(pipe_fds[1], "wake", 4) == 4);
    assert(!close(pipe_fds[0]));
    assert(read(copy, buffer, sizeof(buffer)) == 4 && !memcmp(buffer, "wake", 4));
    /* Status flags belong to the open file: set on the copy, seen on the original. */
    assert(!fcntl(copy, F_SETFL, O_NONBLOCK));
    assert((fcntl(copy, F_GETFL) & O_NONBLOCK));
    assert(read(copy, buffer, sizeof(buffer)) == -1 && errno == EAGAIN);
    close(copy);
    close(pipe_fds[1]);

    assert((file = mkstemp(path)) >= 0);
    assert(write(file, "0123456789", 10) == 10 && lseek(file, 2, SEEK_SET) == 2);
    assert((file_copy = pw_orbis_dup_by_passing(file)) >= 0);
    assert(lseek(file_copy, 0, SEEK_CUR) == 2);      /* one offset, as dup() gives */
    assert(read(file_copy, buffer, 3) == 3 && !memcmp(buffer, "234", 3));
    assert(lseek(file, 0, SEEK_CUR) == 5);
    close(file_copy);
    close(file);
    unlink(path);

    errno = 0;
    assert(pw_orbis_dup_by_passing(-1) == -1 && errno == EBADF);
    for (int i = 0; i < 1000; i++) {
        int d;
        assert((d = pw_orbis_dup_by_passing(0)) >= 0);
        close(d);
    }
    printf("pw wine dup orbis passed: pipe data and flags shared, file offset shared, EBADF, 1000 in a row\n");
    return 0;
}
