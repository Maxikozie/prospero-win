/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Orbis (PS4) replacement for the OpenOrbis SDK's bits/signal.h, ahead of the
 * SDK's and orbis-compat's include directories in Wine's Unix-side build.
 *
 * The SDK ships musl's Linux header over a FreeBSD-9-derived kernel: its
 * signal numbers (SIGBUS 7, SIGUSR1 10, ...), SIG_BLOCK (0), SA_* flags,
 * SS_DISABLE, stack_t and mcontext_t are all Linux's. Wine's signal code is
 * the FreeBSD branch (configure --host=x86_64-unknown-freebsd12) and talks
 * to libkernel's sigaction/sigprocmask/pthread_sigmask, so every value and
 * layout here is FreeBSD amd64's, as sys/sys/signal.h and
 * sys/amd64/include/{signal,ucontext}.h of FreeBSD 9 define them
 * (BSD-2-Clause; the numbers are the kernel ABI, restated). The SDK's own
 * signal.h already carries FreeBSD's siginfo_t (si_addr at +24) and
 * struct sigaction {handler, flags, mask} and keeps musl's 128-byte
 * sigset_t, whose bit layout agrees with FreeBSD's 128-bit set for every
 * signal below 65, so neither is redefined.
 *
 * Measured on the console (the PS4 port's milestone a probe, 2026-10-30, and
 * prospero-win on the PS5): the mcontext sits at ucontext offset 64, the
 * FXSAVE area at 320, the FS base at 1152, cs 0x43, ss 0x3b; the fields up
 * to mc_ss keep FreeBSD's offsets (rip at mcontext 160, mc_addr 136). The
 * region between mc_ownedfp and the FP state is wider than FreeBSD's and
 * is padded here so that uc_mcontext, mc_fpstate and mc_fsbase land on the
 * measured offsets; Wine's patches 0500/0501/0883 use those offsets
 * directly. */
#ifndef _ORBIS_WINE_BITS_SIGNAL_H
#define _ORBIS_WINE_BITS_SIGNAL_H

#if defined(_POSIX_SOURCE) || defined(_POSIX_C_SOURCE) \
 || defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(_BSD_SOURCE)

#if defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
#define MINSIGSTKSZ 2048
#define SIGSTKSZ    34816  /* MINSIGSTKSZ + 32768 */
#endif

/* sigprocmask/pthread_sigmask operations: FreeBSD's, not Linux's 0/1/2. */
#undef SIG_BLOCK
#undef SIG_UNBLOCK
#undef SIG_SETMASK
#define SIG_BLOCK   1
#define SIG_UNBLOCK 2
#define SIG_SETMASK 3

/* si_code values that are not signal specific. */
#undef SI_NOINFO
#undef SI_USER
#undef SI_QUEUE
#undef SI_TIMER
#undef SI_ASYNCIO
#undef SI_MESGQ
#undef SI_KERNEL
#undef SI_LWP
#define SI_NOINFO  0
#define SI_USER    0x10001
#define SI_QUEUE   0x10002
#define SI_TIMER   0x10003
#define SI_ASYNCIO 0x10004
#define SI_MESGQ   0x10005
#define SI_KERNEL  0x10006
#define SI_LWP     0x10007

#if defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
typedef unsigned long __orbis_register_t;

/* FreeBSD amd64 mcontext_t, with the console's FP-state placement. */
typedef struct __mcontext {
    __orbis_register_t mc_onstack;      /*   0: sigstack state to restore */
    __orbis_register_t mc_rdi;          /*   8 */
    __orbis_register_t mc_rsi;          /*  16 */
    __orbis_register_t mc_rdx;          /*  24 */
    __orbis_register_t mc_rcx;          /*  32 */
    __orbis_register_t mc_r8;           /*  40 */
    __orbis_register_t mc_r9;           /*  48 */
    __orbis_register_t mc_rax;          /*  56 */
    __orbis_register_t mc_rbx;          /*  64 */
    __orbis_register_t mc_rbp;          /*  72 */
    __orbis_register_t mc_r10;          /*  80 */
    __orbis_register_t mc_r11;          /*  88 */
    __orbis_register_t mc_r12;          /*  96 */
    __orbis_register_t mc_r13;          /* 104 */
    __orbis_register_t mc_r14;          /* 112 */
    __orbis_register_t mc_r15;          /* 120 */
    unsigned int       mc_trapno;       /* 128 */
    unsigned short     mc_fs;           /* 132 */
    unsigned short     mc_gs;           /* 134 */
    __orbis_register_t mc_addr;         /* 136: fault address */
    unsigned int       mc_flags;        /* 144 */
    unsigned short     mc_es;           /* 148 */
    unsigned short     mc_ds;           /* 150 */
    __orbis_register_t mc_err;          /* 152 */
    __orbis_register_t mc_rip;          /* 160 */
    __orbis_register_t mc_cs;           /* 168 */
    __orbis_register_t mc_rflags;       /* 176 */
    __orbis_register_t mc_rsp;          /* 184 */
    __orbis_register_t mc_ss;           /* 192 */
    long               mc_len;          /* 200 */
    long               mc_fpformat;     /* 208 */
    long               mc_ownedfp;      /* 216 */
    long               __orbis_pad[4];  /* 224: console layout (inferred from the measured offsets) */
    /* 256: FXSAVE (512) + XSAVE header (64) + YMM upper halves (256), as
     * measured at ucontext 320, 832 and 896. */
    long               mc_fpstate[104] __attribute__((aligned(16)));
    __orbis_register_t mc_fsbase;       /* 1088: ucontext 1152, measured */
    __orbis_register_t mc_gsbase;       /* 1096 */
    long               mc_xfpustate;    /* 1104 */
    long               mc_xfpustate_len;/* 1112 */
    long               mc_spare[4];     /* 1120 */
} mcontext_t;                           /* 1152 */
#else
typedef struct { unsigned long __space[144]; } mcontext_t;
#endif

/* FreeBSD's stack_t: ss_size before ss_flags (the SDK has them swapped,
 * which is why its sigaltstack returned EINVAL). */
struct sigaltstack {
    void  *ss_sp;
    size_t ss_size;
    int    ss_flags;
};

/* The console's ucontext: the mcontext at offset 64, measured; the FreeBSD
 * members after it keep their order. Only uc_mcontext is relied upon. */
typedef struct __ucontext {
    unsigned long      __orbis_head[8]; /*   0: uc_sigmask and the kernel's padding */
    mcontext_t         uc_mcontext;     /*  64 */
    struct __ucontext *uc_link;
    stack_t            uc_stack;
    int                uc_flags;
    int                __spare__[4];
} ucontext_t;

#define SA_ONSTACK   0x0001
#define SA_RESTART   0x0002
#define SA_RESETHAND 0x0004
#define SA_NOCLDSTOP 0x0008
#define SA_NODEFER   0x0010
#define SA_NOCLDWAIT 0x0020
#define SA_SIGINFO   0x0040

#define SS_ONSTACK 0x0001
#define SS_DISABLE 0x0004

#endif /* feature test */

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGIOT    SIGABRT
#define SIGEMT    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGBUS    10
#define SIGSEGV   11
#define SIGSYS    12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGURG    16
#define SIGSTOP   17
#define SIGTSTP   18
#define SIGCONT   19
#define SIGCHLD   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGIO     23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGINFO   29
#define SIGUSR1   30
#define SIGUSR2   31
#define SIGTHR    32
#define SIGLWP    SIGTHR
#define SIGLIBRT  33

/* musl's sigset helpers accept signals below _NSIG; FreeBSD's real-time
 * range (65-126) is beyond what a 64-signal set holds and unused here. */
#define _NSIG 65

#endif /* _ORBIS_WINE_BITS_SIGNAL_H */
