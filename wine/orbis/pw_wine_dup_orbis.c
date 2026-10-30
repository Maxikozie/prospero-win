/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * A dup() for the PS4, where dup() is EPERM and fcntl(F_DUPFD) EINVAL on
 * every kind of descriptor (measured in the PS4 port's milestone c, run 10):
 * the descriptor is sent to this process over a socket pair with
 * SCM_RIGHTS, which the same run measured to work, and the kernel installs
 * a new descriptor for the same open file, as dup() does (shared offset and
 * status flags). The server needs one wherever it keeps a descriptor beyond
 * the request that brought it: set_queue_fd keeps the user driver's input
 * pipe so that posted input wakes a GUI thread (milestone d4: the click
 * never arrived while the reopen-by-path fallback, which a pipe has no
 * path for, was all there was), dup_fd_object for mappings.
 */
#include "pw_wine_dup_orbis.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static pthread_mutex_t pass_lock = PTHREAD_MUTEX_INITIALIZER;
static int pass_pair[2] = { -1, -1 };

int pw_orbis_dup_by_passing(int fd)
{
    union { struct cmsghdr header; char space[CMSG_SPACE(sizeof(int))]; } control;
    struct msghdr msg;
    struct iovec iov;
    struct cmsghdr *cmsg;
    char byte = 'd';
    int result = -1, saved = EBADF;

    if (fd < 0) { errno = EBADF; return -1; }
    pthread_mutex_lock(&pass_lock);
    if (pass_pair[0] == -1 && socketpair(AF_UNIX, SOCK_DGRAM, 0, pass_pair)) {
        saved = errno;
        pass_pair[0] = pass_pair[1] = -1;
        goto done;
    }
    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.space;
    msg.msg_controllen = CMSG_SPACE(sizeof(int));
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    if (sendmsg(pass_pair[0], &msg, 0) != 1) { saved = errno; goto done; }

    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.space;
    msg.msg_controllen = sizeof(control.space);
    if (recvmsg(pass_pair[1], &msg, 0) != 1) { saved = errno; goto done; }
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
            memcpy(&result, CMSG_DATA(cmsg), sizeof(int));
    if (result < 0) saved = EMFILE;
done:
    pthread_mutex_unlock(&pass_lock);
    if (result < 0) errno = saved;
    return result;
}
