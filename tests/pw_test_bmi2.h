/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Whether the translator takes its BMI2 encodings on this host, as
 * pw_x86_reencode.c host_bmi2 decides: never when built -DPW_X86_NO_BMI2
 * (make test-nobmi2), and the PS4's Jaguar has none. Without BMI2, high
 * bytes beside memory and bit strings on memory are the emitter's. */
#ifndef PW_TEST_BMI2_H
#define PW_TEST_BMI2_H

static inline int host_has_bmi2(void)
{
#if defined(PW_X86_NO_BMI2) || !defined(__x86_64__)
    return 0;
#else
    unsigned leaf = 0, ebx, ecx = 0, edx;
    __asm__ volatile("cpuid" : "+a"(leaf), "=b"(ebx), "+c"(ecx), "=d"(edx));
    if (leaf < 7) return 0;
    leaf = 7; ecx = 0;
    __asm__ volatile("cpuid" : "+a"(leaf), "=b"(ebx), "+c"(ecx), "=d"(edx));
    return (ebx >> 8) & 1;
#endif
}

#endif
