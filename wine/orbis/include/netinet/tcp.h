/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* FreeBSD 9's netinet/tcp.h for Wine's Unix side on the PS4 (BSD-2-Clause
 * facts, restated): the TCP option numbers and struct tcp_info the kernel
 * knows, not Linux's. */
#ifndef _ORBIS_WINE_NETINET_TCP_H
#define _ORBIS_WINE_NETINET_TCP_H

#include <stdint.h>

#define TCP_NODELAY    1    /* don't delay send to coalesce packets */
#define TCP_MAXSEG     2    /* set maximum segment size */
#define TCP_NOPUSH     4    /* don't push last block of write */
#define TCP_NOOPT      8    /* don't use TCP options */
#define TCP_MD5SIG     16   /* use MD5 digests (RFC2385) */
#define TCP_INFO       32   /* retrieve tcp_info structure */
#define TCP_CONGESTION 64   /* get/set congestion control algorithm */
#define TCP_KEEPINIT   128  /* N, time to establish connection */
#define TCP_KEEPIDLE   256  /* L,N,X start keeplives after this period */
#define TCP_KEEPINTVL  512  /* L,N interval between keepalives */
#define TCP_KEEPCNT    1024 /* L,N number of keepalives before close */

#define TCP_CA_NAME_MAX 16

#define TCPI_OPT_TIMESTAMPS 0x01
#define TCPI_OPT_SACK       0x02
#define TCPI_OPT_WSCALE     0x04
#define TCPI_OPT_ECN        0x08
#define TCPI_OPT_TOE        0x10

struct tcp_info {
    uint8_t  tcpi_state;           /* TCP FSM state */
    uint8_t  __tcpi_ca_state;
    uint8_t  __tcpi_retransmits;
    uint8_t  __tcpi_probes;
    uint8_t  __tcpi_backoff;
    uint8_t  tcpi_options;         /* Options enabled on conn */
    uint8_t  tcpi_snd_wscale:4,    /* RFC1323 send shift value */
             tcpi_rcv_wscale:4;    /* RFC1323 recv shift value */

    uint32_t tcpi_rto;             /* Retransmission timeout (usec) */
    uint32_t __tcpi_ato;
    uint32_t tcpi_snd_mss;         /* Max segment size for send */
    uint32_t tcpi_rcv_mss;         /* Max segment size for receive */

    uint32_t __tcpi_unacked;
    uint32_t __tcpi_sacked;
    uint32_t __tcpi_lost;
    uint32_t __tcpi_retrans;
    uint32_t __tcpi_fackets;

    uint32_t __tcpi_last_data_sent;
    uint32_t __tcpi_last_ack_sent;
    uint32_t tcpi_last_data_recv;  /* Time since last recv data (usec) */
    uint32_t __tcpi_last_ack_recv;

    uint32_t __tcpi_pmtu;
    uint32_t __tcpi_rcv_ssthresh;
    uint32_t tcpi_rtt;             /* Smoothed RTT in usecs */
    uint32_t tcpi_rttvar;          /* RTT variance in usecs */
    uint32_t tcpi_snd_ssthresh;    /* Slow start threshold */
    uint32_t tcpi_snd_cwnd;        /* Send congestion window */
    uint32_t __tcpi_advmss;
    uint32_t __tcpi_reordering;

    uint32_t __tcpi_rcv_rtt;
    uint32_t tcpi_rcv_space;       /* Advertised recv window */

    uint32_t tcpi_snd_wnd;         /* Advertised send window */
    uint32_t tcpi_snd_bwnd;        /* No longer used */
    uint32_t tcpi_snd_nxt;         /* Next egress seqno */
    uint32_t tcpi_rcv_nxt;         /* Next ingress seqno */
    uint32_t tcpi_toe_tid;         /* HWTID for TOE endpoints */
    uint32_t tcpi_snd_rexmitpack;  /* Retransmitted packets */
    uint32_t tcpi_rcv_ooopack;     /* Out-of-order packets */
    uint32_t tcpi_snd_zerowin;     /* Zero-sized windows sent */

    uint32_t __tcpi_pad[26];       /* Padding */
};

#endif /* _ORBIS_WINE_NETINET_TCP_H */
