/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* FreeBSD 9's netinet/in.h and netinet6/in6.h for Wine's Unix side on the
 * PS4 (BSD-2-Clause facts, restated), replacing the SDK's musl header: the
 * socket addresses carry a length byte, and the IP_ and IPV6_ option
 * numbers are the kernel's, not Linux's. */
#ifndef _ORBIS_WINE_NETINET_IN_H
#define _ORBIS_WINE_NETINET_IN_H

#include <sys/socket.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    uint8_t        sin_len;
    sa_family_t    sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    char           sin_zero[8];
};

struct in6_addr {
    union {
        uint8_t  __u6_addr8[16];
        uint16_t __u6_addr16[8];
        uint32_t __u6_addr32[4];
    } __u6_addr;
};
#define s6_addr   __u6_addr.__u6_addr8
#define s6_addr16 __u6_addr.__u6_addr16
#define s6_addr32 __u6_addr.__u6_addr32

struct sockaddr_in6 {
    uint8_t         sin6_len;
    sa_family_t     sin6_family;
    in_port_t       sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;
#define IN6ADDR_ANY_INIT      {{{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 }}}
#define IN6ADDR_LOOPBACK_INIT {{{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 }}}

/* Protocols */
#define IPPROTO_IP      0
#define IPPROTO_HOPOPTS 0
#define IPPROTO_ICMP    1
#define IPPROTO_IGMP    2
#define IPPROTO_IPV4    4
#define IPPROTO_IPIP    IPPROTO_IPV4
#define IPPROTO_TCP     6
#define IPPROTO_EGP     8
#define IPPROTO_PUP     12
#define IPPROTO_UDP     17
#define IPPROTO_IDP     22
#define IPPROTO_TP      29
#define IPPROTO_IPV6    41
#define IPPROTO_ROUTING 43
#define IPPROTO_FRAGMENT 44
#define IPPROTO_RSVP    46
#define IPPROTO_GRE     47
#define IPPROTO_ESP     50
#define IPPROTO_AH      51
#define IPPROTO_ICMPV6  58
#define IPPROTO_NONE    59
#define IPPROTO_DSTOPTS 60
#define IPPROTO_SCTP    132
#define IPPROTO_RAW     255
#define IPPROTO_MAX     256
#define IPPROTO_DONE    257

#define INADDR_ANY       (uint32_t)0x00000000
#define INADDR_BROADCAST (uint32_t)0xffffffff
#define INADDR_LOOPBACK  (uint32_t)0x7f000001
#define INADDR_NONE      (uint32_t)0xffffffff
#define INADDR_UNSPEC_GROUP    (uint32_t)0xe0000000
#define INADDR_ALLHOSTS_GROUP  (uint32_t)0xe0000001
#define INADDR_ALLRTRS_GROUP   (uint32_t)0xe0000002
#define INADDR_MAX_LOCAL_GROUP (uint32_t)0xe00000ff

#define IN_CLASSA(i)     (((uint32_t)(i) & 0x80000000) == 0)
#define IN_CLASSB(i)     (((uint32_t)(i) & 0xc0000000) == 0x80000000)
#define IN_CLASSC(i)     (((uint32_t)(i) & 0xe0000000) == 0xc0000000)
#define IN_CLASSD(i)     (((uint32_t)(i) & 0xf0000000) == 0xe0000000)
#define IN_MULTICAST(i)  IN_CLASSD(i)
#define IN_LOOPBACK(i)   (((uint32_t)(i) & 0xff000000) == 0x7f000000)

/* Options for use with [gs]etsockopt at the IP level */
#define IP_OPTIONS         1
#define IP_HDRINCL         2
#define IP_TOS             3
#define IP_TTL             4
#define IP_RECVOPTS        5
#define IP_RECVRETOPTS     6
#define IP_RECVDSTADDR     7
#define IP_SENDSRCADDR     IP_RECVDSTADDR
#define IP_RETOPTS         8
#define IP_MULTICAST_IF    9
#define IP_MULTICAST_TTL   10
#define IP_MULTICAST_LOOP  11
#define IP_ADD_MEMBERSHIP  12
#define IP_DROP_MEMBERSHIP 13
#define IP_MULTICAST_VIF   14
#define IP_RSVP_ON         15
#define IP_RSVP_OFF        16
#define IP_RSVP_VIF_ON     17
#define IP_RSVP_VIF_OFF    18
#define IP_PORTRANGE       19
#define IP_RECVIF          20
#define IP_IPSEC_POLICY    21
#define IP_FAITH           22
#define IP_ONESBCAST       23
#define IP_BINDANY         24
#define IP_RECVTTL         65
#define IP_MINTTL          66
#define IP_DONTFRAG        67
#define IP_RECVTOS         68
#define IP_ADD_SOURCE_MEMBERSHIP  70
#define IP_DROP_SOURCE_MEMBERSHIP 71
#define IP_BLOCK_SOURCE           72
#define IP_UNBLOCK_SOURCE         73
#define IP_MSFILTER               74

#define IP_DEFAULT_MULTICAST_TTL  1
#define IP_DEFAULT_MULTICAST_LOOP 1
#define IP_MAX_MEMBERSHIPS        4095

struct ip_mreq {
    struct in_addr imr_multiaddr;  /* IP multicast address of group */
    struct in_addr imr_interface;  /* local IP address of interface */
};

struct ip_mreqn {
    struct in_addr imr_multiaddr;
    struct in_addr imr_address;
    int            imr_ifindex;
};

struct ip_mreq_source {
    struct in_addr imr_multiaddr;
    struct in_addr imr_sourceaddr;
    struct in_addr imr_interface;
};

/* IPv6 options */
#define IPV6_SOCKOPT_RESERVED1 3
#define IPV6_UNICAST_HOPS      4
#define IPV6_MULTICAST_IF      9
#define IPV6_MULTICAST_HOPS    10
#define IPV6_MULTICAST_LOOP    11
#define IPV6_JOIN_GROUP        12
#define IPV6_LEAVE_GROUP       13
#define IPV6_ADD_MEMBERSHIP    IPV6_JOIN_GROUP
#define IPV6_DROP_MEMBERSHIP   IPV6_LEAVE_GROUP
#define IPV6_PORTRANGE         14
#define ICMP6_FILTER           18
#define IPV6_CHECKSUM          26
#define IPV6_V6ONLY            27
#define IPV6_IPSEC_POLICY      28
#define IPV6_FAITH             29
#define IPV6_RTHDRDSTOPTS      35
#define IPV6_RECVPKTINFO       36
#define IPV6_RECVHOPLIMIT      37
#define IPV6_RECVRTHDR         38
#define IPV6_RECVHOPOPTS       39
#define IPV6_RECVDSTOPTS       40
#define IPV6_USE_MIN_MTU       42
#define IPV6_RECVPATHMTU       43
#define IPV6_PATHMTU           44
#define IPV6_PKTINFO           46
#define IPV6_HOPLIMIT          47
#define IPV6_NEXTHOP           48
#define IPV6_HOPOPTS           49
#define IPV6_DSTOPTS           50
#define IPV6_RTHDR             51
#define IPV6_RECVTCLASS        57
#define IPV6_AUTOFLOWLABEL     59
#define IPV6_TCLASS            61
#define IPV6_DONTFRAG          62
#define IPV6_PREFER_TEMPADDR   63
#define IPV6_BINDANY           64
#define IPV6_MSFILTER          74

#define IPV6_DEFAULT_MULTICAST_HOPS 1
#define IPV6_DEFAULT_MULTICAST_LOOP 1

struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned int    ipv6mr_interface;
};

struct in6_pktinfo {
    struct in6_addr ipi6_addr;     /* src/dst IPv6 address */
    unsigned int    ipi6_ifindex;  /* send/recv interface index */
};

#define IN6_IS_ADDR_UNSPECIFIED(a) \
    ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && (a)->s6_addr32[2] == 0 && (a)->s6_addr32[3] == 0)
#define IN6_IS_ADDR_LOOPBACK(a) \
    ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && (a)->s6_addr32[2] == 0 && (a)->s6_addr[12] == 0 && \
     (a)->s6_addr[13] == 0 && (a)->s6_addr[14] == 0 && (a)->s6_addr[15] == 1)
#define IN6_IS_ADDR_V4MAPPED(a) \
    ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && (a)->s6_addr[8] == 0 && (a)->s6_addr[9] == 0 && \
     (a)->s6_addr[10] == 0xff && (a)->s6_addr[11] == 0xff)
#define IN6_IS_ADDR_V4COMPAT(a) \
    ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && (a)->s6_addr32[2] == 0 && \
     (a)->s6_addr32[3] != 0 && (a)->s6_addr32[3] != __builtin_bswap32(1))
#define IN6_IS_ADDR_LINKLOCAL(a) (((a)->s6_addr[0] == 0xfe) && (((a)->s6_addr[1] & 0xc0) == 0x80))
#define IN6_IS_ADDR_SITELOCAL(a) (((a)->s6_addr[0] == 0xfe) && (((a)->s6_addr[1] & 0xc0) == 0xc0))
#define IN6_IS_ADDR_MULTICAST(a) ((a)->s6_addr[0] == 0xff)
#define IN6_ARE_ADDR_EQUAL(a, b) (__builtin_memcmp((a), (b), sizeof(struct in6_addr)) == 0)

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

/* The byte-order functions, as arpa/inet.h declares them. */
uint32_t htonl(uint32_t);
uint16_t htons(uint16_t);
uint32_t ntohl(uint32_t);
uint16_t ntohs(uint16_t);

#ifdef __cplusplus
}
#endif

#endif /* _ORBIS_WINE_NETINET_IN_H */
