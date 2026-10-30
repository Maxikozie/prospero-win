/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Orbis (PS4) signal.h for Wine's Unix side: the SDK's header (through
 * orbis-compat's, which fixes sa_sigaction and sigaltstack) with FreeBSD's
 * values for what the SDK defines after bits/signal.h. See bits/signal.h. */
#ifndef _ORBIS_WINE_SIGNAL_H
#define _ORBIS_WINE_SIGNAL_H

#include_next <signal.h>

/* FreeBSD numbers these two the other way round from Linux; the rest of
 * the FPE_, ILL_, SEGV_, BUS_, TRAP_ and CLD_ codes agree. */
#undef FPE_INTOVF
#undef FPE_INTDIV
#define FPE_INTOVF 1
#define FPE_INTDIV 2

#ifndef TRAP_BRKPT
#define TRAP_BRKPT 1
#define TRAP_TRACE 2
#endif

#endif /* _ORBIS_WINE_SIGNAL_H */
