/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A 32-bit console program for the PS4 port's milestone f1: Wine's WoW64
 * runs it on the CPU backend the prefix names (wowprospero: the IA-32
 * translator). It checks, and prints as it goes:
 *
 *   HELLO32 fp=ok thread=ok
 *     fp: x87 (fldpi, fsqrt, fild/fimul/fistp, fsin, fcomi) and SSE2
 *         (sqrtpd, cvttsd2si, cvtsd2si, pmuludq, paddusb, mulsd) give the
 *         processor's results, bit for bit (fsin within one ulp);
 *     thread: a second thread (CreateThread) computes, keeps its own TLS
 *         value, and returns its exit code;
 *   HELLO32S seh=ok
 *     a NULL read caught by a frame handler on the fs:[0] chain (the shape
 *     MSVC's __try produces), a NULL write caught by a vectored handler, and
 *     a NULL read caught by a frame handler on a second thread; each handler
 *     checks the access violation's parameters and resumes after the access;
 *   Hello from PE32
 *
 * A failing check prints what it saw instead of "ok" (fp=x87:2, seh=veh:0,
 * ...). Exit 0 when every check passed, 1 otherwise. Built with
 * i686-w64-mingw32-gcc -O1 -fno-omit-frame-pointer; tests/test_wine_wow32.py
 * runs it under the host Wine, and tests/wine_wow32.c puts it in front of
 * the 32-bit GDI picture for the console's one launch. */
#include <windows.h>
#include <emmintrin.h>
#include <stdio.h>
#include <string.h>

/* ---- fp ------------------------------------------------------------------ */

static unsigned long long bits_of(double value)
{
    unsigned long long bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* The first failing x87 check (1-based), 0 when all passed. */
static int x87_check(void)
{
    double pi, root2, sine, half = 0.5;
    int product, a = 12345, b = 6789, below;
    unsigned long long sine_bits, want_sine = 0x3fdeaee8744b05f0ull;   /* sin(0.5) */

    __asm__ volatile("fldpi\n\tfstpl %0" : "=m"(pi));
    __asm__ volatile("fld1\n\tfadd %%st(0), %%st(0)\n\tfsqrt\n\tfstpl %0" : "=m"(root2));
    __asm__ volatile("fildl %1\n\tfimull %2\n\tfistpl %0" : "=m"(product) : "m"(a), "m"(b));
    __asm__ volatile("fldl %1\n\tfsin\n\tfstpl %0" : "=m"(sine) : "m"(half));
    /* 0 < 1: fcomi sets CF, which setb reads */
    __asm__ volatile("fld1\n\tfldz\n\tfcomip %%st(1), %%st(0)\n\tfstp %%st(0)\n\tsetb %b0\n\tmovzbl %b0, %0"
                     : "=q"(below) : : "cc");
    sine_bits = bits_of(sine);
    if (bits_of(pi) != 0x400921fb54442d18ull) return 1;
    if (bits_of(root2) != 0x3ff6a09e667f3bcdull) return 2;
    if (product != 83810205) return 3;
    if (sine_bits != want_sine && sine_bits + 1 != want_sine && sine_bits != want_sine + 1) return 4;
    if (below != 1) return 5;
    return 0;
}

/* The first failing SSE2 check (1-based), 0 when all passed. */
__attribute__((target("sse2"))) static int sse2_check(void)
{
    double roots[2], squared;
    unsigned long long product[2];
    unsigned char sums[16];
    volatile double two_point_five = 2.5, minus = -2.75, eleven = 1.1;
    __m128d r = _mm_sqrt_pd(_mm_set_pd(9.0, 2.0));
    __m128i ones = _mm_set1_epi32(-1), bytes = _mm_set1_epi8((char)0xf0);

    _mm_storeu_pd(roots, r);
    _mm_storeu_si128((__m128i *)product, _mm_mul_epu32(ones, ones));
    _mm_storeu_si128((__m128i *)sums, _mm_adds_epu8(bytes, _mm_set1_epi8(0x20)));
    squared = _mm_cvtsd_f64(_mm_mul_sd(_mm_set_sd(eleven), _mm_set_sd(eleven)));
    if (bits_of(roots[0]) != 0x3ff6a09e667f3bcdull || roots[1] != 3.0) return 1;
    if (_mm_cvttsd_si32(_mm_set_sd(minus)) != -2) return 2;
    if (_mm_cvtsd_si32(_mm_set_sd(two_point_five)) != 2) return 3;   /* round to even */
    if (product[0] != 0xfffffffe00000001ull || product[1] != 0xfffffffe00000001ull) return 4;
    if (sums[0] != 0xff || sums[15] != 0xff) return 5;
    if (bits_of(squared) != 0x3ff35c28f5c28f5dull) return 6;   /* 1.1 * 1.1 */
    return 0;
}

/* ---- thread -------------------------------------------------------------- */

static DWORD tls_slot;

static DWORD WINAPI worker(void *arg)
{
    volatile LONG *sum = arg;
    LONG total = 0;

    TlsSetValue(tls_slot, (void *)0x1234abcd);
    for (LONG i = 1; i <= 1000; i++) total += i;
    *sum = total + (TlsGetValue(tls_slot) == (void *)0x1234abcd ? 0 : 1);
    return 42;
}

static const char *thread_check(void)
{
    static char failed[48];
    volatile LONG sum = 0;
    DWORD code = 0;
    HANDLE thread;

    tls_slot = TlsAlloc();
    if (tls_slot == TLS_OUT_OF_INDEXES) return "tls";
    TlsSetValue(tls_slot, (void *)0x5a5a5a5a);
    if (!(thread = CreateThread(NULL, 0, worker, (void *)&sum, 0, NULL))) {
        snprintf(failed, sizeof(failed), "create:%lu", GetLastError());
        return failed;
    }
    if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) return "wait";
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    if (code != 42) {
        snprintf(failed, sizeof(failed), "exit:%lu", code);
        return failed;
    }
    if (sum != 500500) {
        snprintf(failed, sizeof(failed), "sum:%ld", (long)sum);
        return failed;
    }
    if (TlsGetValue(tls_slot) != (void *)0x5a5a5a5a) return "tls_main";
    return "ok";
}

/* ---- seh ----------------------------------------------------------------- */

/* Where each handler resumes: the instruction after the faulting access,
 * set by the access's own asm block. */
static volatile DWORD frame_resume, veh_resume;
static volatile LONG frame_hits, veh_hits;

static EXCEPTION_DISPOSITION __cdecl null_frame_handler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context,
                                                        void *dispatcher)
{
    (void)frame; (void)dispatcher;
    if (record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
        record->ExceptionInformation[0] != 0 || record->ExceptionInformation[1] != 0)
        return ExceptionContinueSearch;
    InterlockedIncrement(&frame_hits);
    context->Eip = frame_resume;
    return ExceptionContinueExecution;
}

/* A NULL read under a frame handler on this thread's fs:[0] chain; the
 * record is pushed, used and popped in one asm block, so no compiler code
 * runs with it on the stack. Returns the read's destination, untouched. */
static int frame_null_read(void)
{
    int value = 0x1234;

    __asm__ volatile(
        "pushl %[handler]\n\t"
        "pushl %%fs:0\n\t"
        "movl %%esp, %%fs:0\n\t"
        "movl $1f, %[resume]\n\t"
        "movl 0, %[value]\n\t"
        "1:\n\t"
        "movl (%%esp), %%ecx\n\t"
        "movl %%ecx, %%fs:0\n\t"
        "addl $8, %%esp"
        : [value] "+r"(value), [resume] "=m"(frame_resume)
        : [handler] "r"(null_frame_handler)
        : "ecx", "memory");
    return value;
}

static LONG CALLBACK null_write_handler(EXCEPTION_POINTERS *pointers)
{
    EXCEPTION_RECORD *record = pointers->ExceptionRecord;

    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
        record->ExceptionInformation[0] != 1 || record->ExceptionInformation[1] != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&veh_hits);
    pointers->ContextRecord->Eip = veh_resume;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void veh_null_write(void)
{
    int value = 0x5678;

    __asm__ volatile("movl $1f, %[resume]\n\tmovl %[value], 0\n\t1:"
                     : [resume] "=m"(veh_resume) : [value] "r"(value) : "memory");
}

static DWORD WINAPI seh_worker(void *arg)
{
    (void)arg;
    return frame_null_read() == 0x1234 ? 7 : 8;
}

static const char *seh_check(void)
{
    static char failed[64];
    LONG frames_before;
    DWORD code = 0;
    void *veh;
    HANDLE thread;
    int read;

    read = frame_null_read();
    if (frame_hits != 1 || read != 0x1234) {
        snprintf(failed, sizeof(failed), "frame:%ld", (long)frame_hits);
        return failed;
    }
    if (!(veh = AddVectoredExceptionHandler(1, null_write_handler))) return "veh:add";
    veh_null_write();
    RemoveVectoredExceptionHandler(veh);
    if (veh_hits != 1) {
        snprintf(failed, sizeof(failed), "veh:%ld", (long)veh_hits);
        return failed;
    }
    frames_before = frame_hits;
    if (!(thread = CreateThread(NULL, 0, seh_worker, NULL, 0, NULL))) return "thread:create";
    if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) return "thread:wait";
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    if (code != 7 || frame_hits != frames_before + 1) {
        snprintf(failed, sizeof(failed), "thread:%lu:%ld", code, (long)(frame_hits - frames_before));
        return failed;
    }
    return "ok";
}

int main(void)
{
    char fp[32];
    const char *thread, *seh;
    int x87 = x87_check(), sse2 = sse2_check(), ok;

    if (x87) snprintf(fp, sizeof(fp), "x87:%d", x87);
    else if (sse2) snprintf(fp, sizeof(fp), "sse2:%d", sse2);
    else snprintf(fp, sizeof(fp), "ok");
    thread = thread_check();
    printf("HELLO32 fp=%s thread=%s\n", fp, thread);
    fflush(stdout);
    seh = seh_check();
    printf("HELLO32S seh=%s\n", seh);
    puts("Hello from PE32");
    fflush(stdout);
    ok = !x87 && !sse2 && !strcmp(thread, "ok") && !strcmp(seh, "ok");
    return ok ? 0 : 1;
}
