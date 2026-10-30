/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* FreeBSD 9's sys/socket.h for Wine's Unix side on the PS4 (BSD-2-Clause
 * facts, restated), replacing the SDK's musl header, which carries Linux's
 * layouts and numbers over a FreeBSD kernel:
 *   - struct sockaddr begins with sa_len; a 16-bit sa_family written over
 *     it reaches the kernel as sa_len 2, family 0 (AF_UNSPEC);
 *   - struct msghdr has int msg_iovlen and socklen_t msg_controllen, 48
 *     bytes, not 56; struct cmsghdr is 12 bytes with cmsg_level at +4, not
 *     16 with it at +8: Linux's layout makes SCM_RIGHTS passing (what the
 *     in-process wineserver hands its clients) carry no fd;
 *   - SOL_SOCKET is 0xffff and the SO_ numbers differ throughout;
 *   - Linux's SO_ATTACH_FILTER and SO_BINDTODEVICE do not exist, so Wine's
 *     server does not compile its BPF socket filter.
 * libkernel exports the BSD socket calls; nothing here is a system call. */
#ifndef _ORBIS_WINE_SYS_SOCKET_H
#define _ORBIS_WINE_SYS_SOCKET_H

#include <features.h>
#define __NEED_socklen_t
#define __NEED_sa_family_t
#define __NEED_size_t
#define __NEED_ssize_t
#define __NEED_uid_t
#define __NEED_pid_t
#define __NEED_gid_t
#define __NEED_struct_iovec
#include <bits/alltypes.h>
#include <stdint.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __DEFINED_sa_family_t
typedef unsigned char sa_family_t;   /* FreeBSD: one byte, after sa_len */
#define __DEFINED_sa_family_t
#endif

/* Types */
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_RDM       4
#define SOCK_SEQPACKET 5

/* Option flags per-socket */
#define SO_DEBUG        0x0001
#define SO_ACCEPTCONN   0x0002
#define SO_REUSEADDR    0x0004
#define SO_KEEPALIVE    0x0008
#define SO_DONTROUTE    0x0010
#define SO_BROADCAST    0x0020
#define SO_USELOOPBACK  0x0040
#define SO_LINGER       0x0080
#define SO_OOBINLINE    0x0100
#define SO_REUSEPORT    0x0200
#define SO_TIMESTAMP    0x0400
#define SO_NOSIGPIPE    0x0800
#define SO_ACCEPTFILTER 0x1000
#define SO_BINTIME      0x2000
#define SO_NO_OFFLOAD   0x4000
#define SO_NO_DDP       0x8000

/* Additional options, not kept in so_options */
#define SO_SNDBUF        0x1001
#define SO_RCVBUF        0x1002
#define SO_SNDLOWAT      0x1003
#define SO_RCVLOWAT      0x1004
#define SO_SNDTIMEO      0x1005
#define SO_RCVTIMEO      0x1006
#define SO_ERROR         0x1007
#define SO_TYPE          0x1008
#define SO_LABEL         0x1009
#define SO_PEERLABEL     0x1010
#define SO_LISTENQLIMIT  0x1011
#define SO_LISTENQLEN    0x1012
#define SO_LISTENINCQLEN 0x1013
#define SO_SETFIB        0x1014
#define SO_USER_COOKIE   0x1015
#define SO_PROTOCOL      0x1016
#define SO_PROTOTYPE     SO_PROTOCOL

struct linger {
    int l_onoff;    /* option on/off */
    int l_linger;   /* linger time */
};

struct accept_filter_arg {
    char af_name[16];
    char af_arg[256 - 16];
};

#define SOL_SOCKET 0xffff   /* options for socket level */

/* Address families */
#define AF_UNSPEC      0
#define AF_UNIX        1
#define AF_LOCAL       AF_UNIX
#define AF_INET        2
#define AF_IMPLINK     3
#define AF_PUP         4
#define AF_CHAOS       5
#define AF_NETBIOS     6
#define AF_ISO         7
#define AF_OSI         AF_ISO
#define AF_ECMA        8
#define AF_DATAKIT     9
#define AF_CCITT       10
#define AF_SNA         11
#define AF_DECnet      12
#define AF_DLI         13
#define AF_LAT         14
#define AF_HYLINK      15
#define AF_APPLETALK   16
#define AF_ROUTE       17
#define AF_LINK        18
#define pseudo_AF_XTP  19
#define AF_COIP        20
#define AF_CNT         21
#define pseudo_AF_RTIP 22
#define AF_IPX         23
#define AF_SIP         24
#define pseudo_AF_PIP  25
#define AF_ISDN        26
#define AF_E164        AF_ISDN
#define pseudo_AF_KEY  27
#define AF_INET6       28
#define AF_NATM        29
#define AF_ATM         30
#define pseudo_AF_HDRCMPLT 31
#define AF_NETGRAPH    32
#define AF_SLOW        33
#define AF_SCLUSTER    34
#define AF_ARP         35
#define AF_BLUETOOTH   36
#define AF_IEEE80211   37
#define AF_MAX         38

struct sockaddr {
    unsigned char sa_len;       /* total length */
    sa_family_t   sa_family;    /* address family */
    char          sa_data[14];  /* actually longer; address value */
};
#define SOCK_MAXADDRLEN 255

#define _SS_MAXSIZE  128U
#define _SS_ALIGNSIZE (sizeof(int64_t))
#define _SS_PAD1SIZE (_SS_ALIGNSIZE - sizeof(unsigned char) - sizeof(sa_family_t))
#define _SS_PAD2SIZE (_SS_MAXSIZE - sizeof(unsigned char) - sizeof(sa_family_t) - _SS_PAD1SIZE - _SS_ALIGNSIZE)

struct sockaddr_storage {
    unsigned char ss_len;
    sa_family_t   ss_family;
    char          __ss_pad1[_SS_PAD1SIZE];
    int64_t       __ss_align;
    char          __ss_pad2[_SS_PAD2SIZE];
};

/* Protocol families, same as address families */
#define PF_UNSPEC    AF_UNSPEC
#define PF_LOCAL     AF_LOCAL
#define PF_UNIX      PF_LOCAL
#define PF_INET      AF_INET
#define PF_IMPLINK   AF_IMPLINK
#define PF_PUP       AF_PUP
#define PF_CHAOS     AF_CHAOS
#define PF_NETBIOS   AF_NETBIOS
#define PF_ISO       AF_ISO
#define PF_OSI       AF_ISO
#define PF_ECMA      AF_ECMA
#define PF_DATAKIT   AF_DATAKIT
#define PF_CCITT     AF_CCITT
#define PF_SNA       AF_SNA
#define PF_DECnet    AF_DECnet
#define PF_DLI       AF_DLI
#define PF_LAT       AF_LAT
#define PF_HYLINK    AF_HYLINK
#define PF_APPLETALK AF_APPLETALK
#define PF_ROUTE     AF_ROUTE
#define PF_LINK      AF_LINK
#define PF_XTP       pseudo_AF_XTP
#define PF_COIP      AF_COIP
#define PF_CNT       AF_CNT
#define PF_SIP       AF_SIP
#define PF_IPX       AF_IPX
#define PF_RTIP      pseudo_AF_RTIP
#define PF_PIP       pseudo_AF_PIP
#define PF_ISDN      AF_ISDN
#define PF_KEY       pseudo_AF_KEY
#define PF_INET6     AF_INET6
#define PF_NATM      AF_NATM
#define PF_ATM       AF_ATM
#define PF_NETGRAPH  AF_NETGRAPH
#define PF_SLOW      AF_SLOW
#define PF_SCLUSTER  AF_SCLUSTER
#define PF_ARP       AF_ARP
#define PF_BLUETOOTH AF_BLUETOOTH
#define PF_MAX       AF_MAX

#define SOMAXCONN 128

struct msghdr {
    void         *msg_name;       /* optional address */
    socklen_t     msg_namelen;    /* size of address */
    struct iovec *msg_iov;        /* scatter/gather array */
    int           msg_iovlen;     /* # elements in msg_iov */
    void         *msg_control;    /* ancillary data, see below */
    socklen_t     msg_controllen; /* ancillary data buffer len */
    int           msg_flags;      /* flags on received message */
};

#define MSG_OOB          0x1      /* process out-of-band data */
#define MSG_PEEK         0x2      /* peek at incoming message */
#define MSG_DONTROUTE    0x4      /* send without using routing tables */
#define MSG_EOR          0x8      /* data completes record */
#define MSG_TRUNC        0x10     /* data discarded before delivery */
#define MSG_CTRUNC       0x20     /* control data lost before delivery */
#define MSG_WAITALL      0x40     /* wait for full request or error */
#define MSG_DONTWAIT     0x80     /* this message should be nonblocking */
#define MSG_EOF          0x100    /* data completes connection */
#define MSG_NOTIFICATION 0x2000   /* SCTP notification */
#define MSG_NBIO         0x4000   /* FIONBIO mode, used by fifofs */
#define MSG_COMPAT       0x8000   /* used in sendit() */
#define MSG_NOSIGNAL     0x20000  /* do not generate SIGPIPE on EOF */

struct cmsghdr {
    socklen_t cmsg_len;    /* data byte count, including hdr */
    int       cmsg_level;  /* originating protocol */
    int       cmsg_type;   /* protocol-specific type */
    /* followed by u_char cmsg_data[]; */
};

#define __ORBIS_CMSG_ALIGN(n) (((size_t)(n) + 7) & ~(size_t)7)
#define CMSG_DATA(cmsg) ((unsigned char *)(cmsg) + __ORBIS_CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_NXTHDR(mhdr, cmsg) \
    ((char *)(cmsg) == NULL ? CMSG_FIRSTHDR(mhdr) : \
     ((char *)(cmsg) + __ORBIS_CMSG_ALIGN((cmsg)->cmsg_len) + __ORBIS_CMSG_ALIGN(sizeof(struct cmsghdr)) > \
      (char *)(mhdr)->msg_control + (mhdr)->msg_controllen) ? (struct cmsghdr *)0 : \
     (struct cmsghdr *)((char *)(cmsg) + __ORBIS_CMSG_ALIGN((cmsg)->cmsg_len)))
#define CMSG_FIRSTHDR(mhdr) \
    ((mhdr)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr *)(mhdr)->msg_control : (struct cmsghdr *)NULL)
#define CMSG_SPACE(l) (__ORBIS_CMSG_ALIGN(sizeof(struct cmsghdr)) + __ORBIS_CMSG_ALIGN(l))
#define CMSG_LEN(l)   (__ORBIS_CMSG_ALIGN(sizeof(struct cmsghdr)) + (l))

/* "Socket"-level control message types */
#define SCM_RIGHTS    0x01  /* access rights (array of int) */
#define SCM_TIMESTAMP 0x02  /* timestamp (struct timeval) */
#define SCM_CREDS     0x03  /* process creds (struct cmsgcred) */
#define SCM_BINTIME   0x04  /* timestamp (struct bintime) */

/* howto arguments for shutdown(2) */
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

int accept(int, struct sockaddr *__restrict, socklen_t *__restrict);
int bind(int, const struct sockaddr *, socklen_t);
int connect(int, const struct sockaddr *, socklen_t);
int getpeername(int, struct sockaddr *__restrict, socklen_t *__restrict);
int getsockname(int, struct sockaddr *__restrict, socklen_t *__restrict);
int getsockopt(int, int, int, void *__restrict, socklen_t *__restrict);
int listen(int, int);
ssize_t recv(int, void *, size_t, int);
ssize_t recvfrom(int, void *, size_t, int, struct sockaddr *__restrict, socklen_t *__restrict);
ssize_t recvmsg(int, struct msghdr *, int);
ssize_t send(int, const void *, size_t, int);
ssize_t sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t sendmsg(int, const struct msghdr *, int);
int setsockopt(int, int, int, const void *, socklen_t);
int shutdown(int, int);
int sockatmark(int);
int socket(int, int, int);
int socketpair(int, int, int, int *);

#ifdef __cplusplus
}
#endif

#endif /* _ORBIS_WINE_SYS_SOCKET_H */
