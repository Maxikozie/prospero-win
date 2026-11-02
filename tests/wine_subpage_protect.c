/* SPDX-License-Identifier: LGPL-2.1-or-later
 * A Windows page made read-only or inaccessible faults on access even when
 * its host page is larger and holds pages that stay writable (the PS5's are
 * 16 KiB), and those neighbours keep working without faults the program sees.
 * Emulators that recompile code protect the 4 KiB pages holding it and expect
 * the fault that tells them the code changed (DuckStation, for one). */
#include <windows.h>
#include <stdio.h>

#define PAGE 4096
#define PAGES 20

static unsigned char *base;
static volatile LONG faults, fault_page, fault_write;

/* Records the fault and makes the page writable again, as an emulator does
 * after it drops the code it compiled from that page. */
static LONG CALLBACK handler(EXCEPTION_POINTERS *info)
{
    const EXCEPTION_RECORD *rec = info->ExceptionRecord;
    unsigned char *addr = (unsigned char *)rec->ExceptionInformation[1];
    DWORD old;

    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || addr < base || addr >= base + PAGES * PAGE)
        return EXCEPTION_CONTINUE_SEARCH;
    faults++;
    fault_page = (LONG)((addr - base) / PAGE);
    fault_write = rec->ExceptionInformation[0] == 1;
    VirtualProtect(base + fault_page * PAGE, PAGE, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int protect(int page, DWORD prot)
{
    DWORD old;

    if (VirtualProtect(base + page * PAGE, PAGE, prot, &old)) return 1;
    printf("subpage protect %d %#lx failed %lu\n", page, prot, GetLastError());
    return 0;
}

/* Writes one byte to each page in turn; returns how many faults the program saw. */
static LONG write_pages(int first, int count, unsigned char value)
{
    LONG before = faults;
    int page;

    for (page = first; page < first + count; page++) ((volatile unsigned char *)base)[page * PAGE + 123] = value;
    return faults - before;
}

static int expect(const char *what, LONG got, LONG want)
{
    printf("subpage %s: %ld fault(s), expected %ld\n", what, got, want);
    return got == want;
}

int main(void)
{
    LARGE_INTEGER freq, start, end;
    volatile unsigned char sink;
    LONG got;
    int i, ok;

    base = VirtualAlloc(NULL, PAGES * PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ok = base && AddVectoredExceptionHandler(1, handler);

    /* A read-only page inside a host page whose other pages stay writable. */
    ok = ok && protect(1, PAGE_READONLY);
    ok = ok && expect("writes beside a read-only page", write_pages(0, 1, 1) + write_pages(2, 2, 2), 0);
    got = write_pages(1, 1, 3);
    ok = ok && expect("write to the read-only page", got, 1) && fault_page == 1 && fault_write;
    ok = ok && base[PAGE + 123] == 3;
    ok = ok && expect("second write after the handler made it writable", write_pages(1, 1, 4), 0);

    /* Read-only pages on both sides of a host page boundary, writable ones between. */
    ok = ok && protect(3, PAGE_READONLY) && protect(4, PAGE_READONLY);
    ok = ok && expect("writes between read-only pages", write_pages(0, 3, 5) + write_pages(5, 3, 6), 0);
    ok = ok && expect("writes to both read-only pages", write_pages(3, 2, 7), 2);

    /* An inaccessible page faults on a read too. */
    ok = ok && protect(9, PAGE_NOACCESS);
    got = faults;
    sink = base[8 * PAGE] + base[10 * PAGE];
    ok = ok && expect("reads beside an inaccessible page", faults - got, 0);
    got = faults;
    sink = base[9 * PAGE + 7];
    ok = ok && expect("read of the inaccessible page", faults - got, 1) && fault_page == 9 && !fault_write;
    (void)sink;

    /* Many writes beside a read-only page: the first ones are stepped over unseen;
     * when they keep coming, the host page is kept whole and the rest run unstepped.
     * None of them is reported, since none touched the read-only page (a loader
     * filling a page next to a section it made read-only takes a report for a real
     * access violation). Protecting the page again splits the host page: a write to
     * it faults again. */
    ok = ok && protect(13, PAGE_READONLY);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    got = faults;
    for (i = 0; i < 20000; i++) ((volatile unsigned char *)base)[12 * PAGE + (i & 4095)] = (unsigned char)i;
    QueryPerformanceCounter(&end);
    ok = ok && expect("20000 writes beside a read-only page", faults - got, 0);
    printf("subpage neighbour writes: %.2f us each\n",
           (double)(end.QuadPart - start.QuadPart) * 1e6 / freq.QuadPart / 20000);
    ok = ok && base[12 * PAGE + 19999 % 4096] == (unsigned char)19999;
    ok = ok && protect(13, PAGE_READONLY);
    got = write_pages(13, 1, 11);
    ok = ok && expect("write to the read-only page protected again", got, 1) && fault_page == 13 && fault_write;

    /* Pages decommitted beside a read-only page in the same host page: Wine clears
     * them itself, since the host page stays, and that must neither fail nor fault.
     * They come back as zeroes, and the read-only page still faults on a write.
     * A loader that maps a module into memory it allocated does this (the Bloodlines
     * Unofficial Patch's loader.dll). */
    write_pages(16, 4, 8);
    ok = ok && protect(17, PAGE_READONLY);
    got = faults;
    if (ok && !VirtualFree(base + 18 * PAGE, 2 * PAGE, MEM_DECOMMIT))
    {
        printf("subpage decommit beside a read-only page failed %lu\n", GetLastError());
        ok = 0;
    }
    ok = ok && expect("decommit beside a read-only page", faults - got, 0);
    ok = ok && VirtualAlloc(base + 18 * PAGE, 2 * PAGE, MEM_COMMIT, PAGE_READWRITE) == base + 18 * PAGE;
    ok = ok && base[18 * PAGE + 123] == 0 && base[19 * PAGE + 123] == 0 && base[17 * PAGE + 123] == 8;
    ok = ok && expect("writes beside the read-only page after the decommit", write_pages(16, 1, 9) + write_pages(18, 2, 9), 0);
    got = write_pages(17, 1, 10);
    ok = ok && expect("write to the read-only page after the decommit", got, 1) && fault_page == 17 && fault_write;

    printf("subpage verdict=%s\n", ok ? "pass" : "fail");
    fflush(stdout);
    return ok ? 0 : 1;
}
