/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* FreeBSD 9's sys/event.h for Wine's Unix side on the PS4 (BSD-2-Clause
 * facts, restated): libkernel exports kqueue() and kevent(), and the SDK
 * ships no header for them. Wine's server (fd.c) takes the kqueue path when
 * this header exists, as it does on the PS5; whether the console's kqueue
 * answers every filter is a run-time question (prospero-win found vnode
 * notifications missing on the PS5, patch 0884 polls instead). The kevent
 * layout is FreeBSD 9's 32-byte one, without the ext[] words of 11. */
#ifndef _ORBIS_WINE_SYS_EVENT_H
#define _ORBIS_WINE_SYS_EVENT_H

#include <sys/types.h>
#include <stdint.h>

#define EVFILT_READ     (-1)
#define EVFILT_WRITE    (-2)
#define EVFILT_AIO      (-3)    /* attached to aio requests */
#define EVFILT_VNODE    (-4)    /* attached to vnodes */
#define EVFILT_PROC     (-5)    /* attached to struct proc */
#define EVFILT_SIGNAL   (-6)    /* attached to struct proc */
#define EVFILT_TIMER    (-7)    /* timers */
#define EVFILT_FS       (-9)    /* filesystem events */
#define EVFILT_LIO      (-10)   /* attached to lio requests */
#define EVFILT_USER     (-11)   /* user events */
#define EVFILT_SYSCOUNT 11

#define EV_SET(kevp_, a, b, c, d, e, f) do {   \
    struct kevent *kevp = (kevp_);             \
    (kevp)->ident = (a);                       \
    (kevp)->filter = (b);                      \
    (kevp)->flags = (c);                       \
    (kevp)->fflags = (d);                      \
    (kevp)->data = (e);                        \
    (kevp)->udata = (f);                       \
} while (0)

struct kevent {
    uintptr_t      ident;   /* identifier for this event */
    short          filter;  /* filter for event */
    unsigned short flags;
    unsigned int   fflags;
    intptr_t       data;
    void          *udata;   /* opaque user data identifier */
};

/* actions */
#define EV_ADD      0x0001  /* add event to kq (implies enable) */
#define EV_DELETE   0x0002  /* delete event from kq */
#define EV_ENABLE   0x0004  /* enable event */
#define EV_DISABLE  0x0008  /* disable event (not reported) */

/* flags */
#define EV_ONESHOT  0x0010  /* only report one occurrence */
#define EV_CLEAR    0x0020  /* clear event state after reporting */
#define EV_RECEIPT  0x0040  /* force EV_ERROR on success, data=0 */
#define EV_DISPATCH 0x0080  /* disable event after reporting */

#define EV_SYSFLAGS 0xF000  /* reserved by system */
#define EV_FLAG1    0x2000  /* filter-specific flag */

/* returned values */
#define EV_EOF      0x8000  /* EOF detected */
#define EV_ERROR    0x4000  /* error, data contains errno */

/* data/hint flags/masks for EVFILT_USER */
#define NOTE_FFNOP      0x00000000  /* ignore input fflags */
#define NOTE_FFAND      0x40000000  /* AND fflags */
#define NOTE_FFOR       0x80000000  /* OR fflags */
#define NOTE_FFCOPY     0xc0000000  /* copy fflags */
#define NOTE_FFCTRLMASK 0xc0000000  /* masks for operations */
#define NOTE_FFLAGSMASK 0x00ffffff
#define NOTE_TRIGGER    0x01000000  /* cause the event to be triggered */

/* data/hint flags for EVFILT_{READ|WRITE} */
#define NOTE_LOWAT      0x0001      /* low water mark */

/* data/hint flags for EVFILT_VNODE */
#define NOTE_DELETE     0x0001      /* vnode was removed */
#define NOTE_WRITE      0x0002      /* data contents changed */
#define NOTE_EXTEND     0x0004      /* size increased */
#define NOTE_ATTRIB     0x0008      /* attributes changed */
#define NOTE_LINK       0x0010      /* link count changed */
#define NOTE_RENAME     0x0020      /* vnode was renamed */
#define NOTE_REVOKE     0x0040      /* vnode access was revoked */

/* data/hint flags for EVFILT_PROC */
#define NOTE_EXIT       0x80000000  /* process exited */
#define NOTE_FORK       0x40000000  /* process forked */
#define NOTE_EXEC       0x20000000  /* process exec'd */

struct timespec;

#ifdef __cplusplus
extern "C" {
#endif
int kqueue(void);
int kevent(int kq, const struct kevent *changelist, int nchanges, struct kevent *eventlist, int nevents,
           const struct timespec *timeout);
#ifdef __cplusplus
}
#endif

#endif /* _ORBIS_WINE_SYS_EVENT_H */
