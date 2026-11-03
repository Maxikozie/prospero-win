/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A 32-bit console program in the shape of games built without relocations
 * (the PS4 port's milestone i): linked at 0x400000 with no relocation
 * section, so it loads there or not at all, with a 4 MiB array that takes
 * SizeOfImage past 0x40c000 (Psychonauts.exe's) and 4 KiB sections that share
 * the console's 16 KiB pages (.text RX, .rdata R, .data RW), and one read-only
 * section, .ro16, large enough to fill a 16 KiB host page of its own. It prints
 *
 *   FIXED32 bits=32 base=0x00400000 relocs=0 size_kib=N text_rx=ok rdata_ro=ok rdata_shared=faults|writable
 *           data_rw=ok seh=ok thread=ok
 *
 *   base, relocs, size_kib: where its image is, its relocation directory's
 *       size, SizeOfImage in KiB;
 *   text_rx: VirtualQuery of its code says PAGE_EXECUTE_READ;
 *   rdata_ro: a write to the 16 KiB-aligned page inside .ro16 raises an access
 *       violation (write, at that address), caught by a vectored handler that
 *       resumes after it. Its host page holds read-only image pages only;
 *   rdata_shared (recorded): the same write to .rdata, which shares its host
 *       page with .data when host pages are 16 KiB: "faults", or "writable"
 *       where Wine gives the image's host page the union of its pages'
 *       protections (fork patch 0974, by design: sections sharing a host page
 *       cost nothing). Writable is expected on the console;
 *   data_rw: .data and both ends of the 4 MiB .bss array write and read back;
 *   seh: a NULL read caught by a frame handler on the fs:[0] chain;
 *   thread: a second thread computes and returns its exit code.
 *
 * A failing check prints what it saw instead of "ok". Exit 0 when every check
 * passed, 1 otherwise. Built with i686-w64-mingw32-gcc -O1 -s
 * -fno-omit-frame-pointer -Wl,--image-base,0x400000,--disable-dynamicbase,
 * --disable-reloc-section; tests/test_wine_fixed32.py runs it under the host
 * Wine, with 4 KiB and with 16 KiB host pages. The SEH and thread checks
 * follow tests/wine_hello32.c. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define BIG_BYTES (4u * 1024u * 1024u)

static volatile char big[BIG_BYTES];                 /* .bss */
static volatile int data_word = 0x1b1b;              /* .data */
static const volatile int rodata_word[16] = { 0x52 }; /* .rdata */
/* 48 KiB from an 8 KiB boundary (COFF's largest alignment) always holds one
 * whole 16 KiB host page of read-only image memory. */
__attribute__((section(".ro16"), aligned(8192))) static const volatile int ro16_words[12288] = { 0x52 };

/* ---- image ---------------------------------------------------------------- */

static void image_fields(char *base, size_t base_size, unsigned long *relocs, unsigned long *size_kib)
{
    const BYTE *module = (const BYTE *)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(module + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];

    snprintf(base, base_size, "0x%08lx", (unsigned long)(ULONG_PTR)module);
    *relocs = nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC ? dir->Size : 0;
    *size_kib = nt->OptionalHeader.SizeOfImage / 1024;
}

/* ---- text --------------------------------------------------------------------- */

static const char *text_check(void)
{
    static char failed[32];
    MEMORY_BASIC_INFORMATION info;

    if (!VirtualQuery((const void *)(ULONG_PTR)text_check, &info, sizeof(info))) return "query";
    if (info.Protect == PAGE_EXECUTE_READ) return "ok";
    snprintf(failed, sizeof(failed), "0x%lx", (unsigned long)info.Protect);
    return failed;
}

/* ---- rdata ------------------------------------------------------------------ */

static volatile DWORD write_resume;
static volatile LONG write_hits;
static volatile ULONG_PTR write_address;

static LONG CALLBACK rdata_write_handler(EXCEPTION_POINTERS *pointers)
{
    EXCEPTION_RECORD *record = pointers->ExceptionRecord;

    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
        record->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    write_address = record->ExceptionInformation[1];
    InterlockedIncrement(&write_hits);
    pointers->ContextRecord->Eip = write_resume;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* A write of 0x77 at target under a vectored handler: "ok" when it faulted
 * there and the word kept its value, else what was seen. */
static const char *write_faults(volatile int *target, char *failed, size_t size)
{
    int before = *target;
    void *veh;

    write_hits = 0;
    write_address = 0;
    if (!(veh = AddVectoredExceptionHandler(1, rdata_write_handler))) return "veh:add";
    __asm__ volatile("movl $1f, %[resume]\n\tmovl $0x77, (%[target])\n\t1:"
                     : [resume] "=m"(write_resume) : [target] "r"(target) : "memory");
    RemoveVectoredExceptionHandler(veh);
    if (write_hits != 1) {
        snprintf(failed, size, "writable:%ld", (long)write_hits);
        return failed;
    }
    if (write_address != (ULONG_PTR)target) {
        snprintf(failed, size, "address:0x%lx", (unsigned long)write_address);
        return failed;
    }
    if (*target != before) return "changed";
    return "ok";
}

/* rdata_ro: the page of .ro16 that starts on a 16 KiB boundary. */
static const char *rdata_check(void)
{
    static char failed[48];
    ULONG_PTR page = ((ULONG_PTR)&ro16_words[0] + 16383) & ~(ULONG_PTR)16383;

    return write_faults((volatile int *)page, failed, sizeof(failed));
}

/* rdata_shared: .rdata, on a host page it may share with .data. */
static const char *rdata_shared_check(void)
{
    static char failed[48];
    const char *result = write_faults((volatile int *)&rodata_word[3], failed, sizeof(failed));

    if (!strcmp(result, "ok")) return "faults";
    return !strncmp(result, "writable:0", 10) ? "writable" : result;
}

/* ---- data ------------------------------------------------------------------- */

static const char *data_check(void)
{
    data_word = 0x2c2c;
    big[0] = 0x11;
    big[BIG_BYTES / 2] = 0x22;
    big[BIG_BYTES - 1] = 0x33;
    if (data_word != 0x2c2c) return "data";
    if (big[0] != 0x11 || big[BIG_BYTES / 2] != 0x22 || big[BIG_BYTES - 1] != 0x33) return "bss";
    return "ok";
}

/* ---- seh (tests/wine_hello32.c's frame handler) ------------------------------- */

static volatile DWORD frame_resume;
static volatile LONG frame_hits;

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

static const char *seh_check(void)
{
    static char failed[32];
    int read = frame_null_read();

    if (frame_hits != 1 || read != 0x1234) {
        snprintf(failed, sizeof(failed), "frame:%ld", (long)frame_hits);
        return failed;
    }
    return "ok";
}

/* ---- thread ----------------------------------------------------------------- */

static DWORD WINAPI worker(void *arg)
{
    volatile LONG *sum = arg;
    LONG total = 0;

    for (LONG i = 1; i <= 1000; i++) total += i;
    *sum = total;
    return 42;
}

static const char *thread_check(void)
{
    static char failed[48];
    volatile LONG sum = 0;
    DWORD code = 0;
    HANDLE thread;

    if (!(thread = CreateThread(NULL, 0, worker, (void *)&sum, 0, NULL))) {
        snprintf(failed, sizeof(failed), "create:%lu", GetLastError());
        return failed;
    }
    if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) return "wait";
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    if (code != 42 || sum != 500500) {
        snprintf(failed, sizeof(failed), "exit:%lu:%ld", code, (long)sum);
        return failed;
    }
    return "ok";
}

int main(void)
{
    char base[16];
    unsigned long relocs, size_kib;
    const char *text, *rdata, *shared, *data, *seh, *thread;
    int ok;

    image_fields(base, sizeof(base), &relocs, &size_kib);
    text = text_check();
    rdata = rdata_check();
    shared = rdata_shared_check();
    data = data_check();
    seh = seh_check();
    thread = thread_check();
    printf("FIXED32 bits=%d base=%s relocs=%lu size_kib=%lu text_rx=%s rdata_ro=%s rdata_shared=%s data_rw=%s seh=%s "
           "thread=%s\n", (int)(sizeof(void *) * 8), base, relocs, size_kib, text, rdata, shared, data, seh, thread);
    fflush(stdout);
    ok = !strcmp(base, "0x00400000") && !relocs && size_kib >= 4144 && !strcmp(text, "ok") && !strcmp(rdata, "ok") &&
         !strcmp(data, "ok") && !strcmp(seh, "ok") && !strcmp(thread, "ok");
    return ok ? 0 : 1;
}
