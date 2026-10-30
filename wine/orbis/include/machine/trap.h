/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* FreeBSD amd64 trap numbers (sys/amd64/include/trap.h, BSD-2-Clause; the
 * values are the kernel ABI, restated), which the OpenOrbis SDK does not
 * ship. Wine's signal_x86_64.c reads them from mc_trapno; the console's
 * kernel is FreeBSD-derived and prospero-win relies on the same numbers on
 * the PS5. */
#ifndef _ORBIS_WINE_MACHINE_TRAP_H
#define _ORBIS_WINE_MACHINE_TRAP_H

#define T_PRIVINFLT  1   /* privileged instruction */
#define T_BPTFLT     3   /* breakpoint instruction */
#define T_ARITHTRAP  6   /* arithmetic trap */
#define T_PROTFLT    9   /* protection fault */
#define T_TRCTRAP   10   /* debug exception (sic) */
#define T_PAGEFLT   12   /* page fault */
#define T_ALIGNFLT  14   /* alignment fault */

#define T_DIVIDE    18   /* integer divide fault */
#define T_NMI       19   /* non-maskable trap */
#define T_OFLOW     20   /* overflow trap */
#define T_BOUND     21   /* bound instruction */
#define T_DNA       22   /* device not available */
#define T_DOUBLEFLT 23   /* double fault */
#define T_FPOPFLT   24   /* fp coprocessor operand fetch fault */
#define T_TSSFLT    25   /* invalid tss fault */
#define T_SEGNPFLT  26   /* segment not present fault */
#define T_STKFLT    27   /* stack fault */
#define T_MCHK      28   /* machine check trap */
#define T_XMMFLT    29   /* SIMD floating-point exception */
#define T_RESERVED  30   /* reserved (unknown) */
#define T_DTRACE_RET 32  /* DTrace pid return */

#define T_USER      0x100  /* user-mode flag or'd with type */

#endif /* _ORBIS_WINE_MACHINE_TRAP_H */
