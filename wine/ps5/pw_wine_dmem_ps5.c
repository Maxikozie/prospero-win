/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The console side of pw_wine_dmem, linked into ntdll.prx: libkernel's
 * direct memory behind the mmap, munmap and mprotect calls of ntdll's
 * virtual memory manager (patch 0600 routes them here), for the address
 * ranges it reserves up front. Everything else passes straight through.
 *
 * The recipe is the one ps5-xash3d's heap runs on (FW 12.02): main direct
 * memory, mapped read-write at a fixed address over a reservation, then
 * protected with sceKernelMprotect; execute permission is refused at map
 * time but granted by mprotect. A self-check maps, writes, protects and
 * frees one page before the first region is taken; if any step fails, the
 * regions stay ordinary reservations and every call passes through.
 *
 * On the PS4 (__ORBIS__) the same recipe runs over sceKernelAllocateDirectMemory
 * (one pool, WB_ONION) and the same map, protect and reserve calls, which the
 * PS4 port's milestone a probe measured: a 16 KiB granule, 16 GiB reservations,
 * execute granted by mprotect after a read-write map. */
#include "pw_wine_dmem.h"
#include "pw_wine_heap.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

extern int sceKernelReserveVirtualRange(void **address, size_t bytes, int flags, size_t alignment);
#ifdef __ORBIS__
/* The PS4 has one direct-memory pool, allocated by physical range: the whole
 * pool, any free block. Type 0 is SCE_KERNEL_WB_ONION (CPU-cached memory);
 * mapping with the GPU bits and executing after sceKernelMprotect were
 * measured on the console (the PS4 port's milestone a probe). */
extern int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t bytes, size_t alignment, int type,
                                         int64_t *offset);
#else
extern int sceKernelAllocateMainDirectMemory(size_t bytes, size_t alignment, int type, int64_t *offset);
#endif
extern int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags,
                                    int64_t offset, size_t alignment);
extern int sceKernelMprotect(const void *address, size_t bytes, int protection);
extern int sceKernelMunmap(void *address, size_t bytes);
extern int sceKernelReleaseDirectMemory(int64_t offset, size_t bytes);
extern size_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t alignment, int64_t *address,
                                              size_t *bytes);

enum {
    PAGE = 0x4000,
    KERNEL_FIXED = 0x10,
    KERNEL_NO_OVERWRITE = 0x80,
#ifdef __ORBIS__
    MEMORY_TYPE = 0x00,       /* SCE_KERNEL_WB_ONION */
#else
    MEMORY_TYPE = 0x0c,       /* as ps5-xash3d's heap */
#endif
    CPU_READ_WRITE = 0x03,
    CPU_GPU_READ_WRITE = 0xf2,  /* what ps5-xash3d's heap maps with */
    RUNS = 32768,
};

/* WINE_PS5_WAIT_WATCHDOG: a direct-memory call slower than 50 ms is
 * logged (prospero-win#250). */
static uint64_t slow_start(void)
{
    static int enabled = -1;
    struct timespec ts;

    if (enabled == -1) enabled = getenv("WINE_PS5_WAIT_WATCHDOG") != NULL;
    if (!enabled) return 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static void slow_end(uint64_t start, const char *what, size_t bytes)
{
    struct timespec ts;
    uint64_t now;

    if (!start) return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    if (now - start > 50000)
        fprintf(stderr, "wine-ps5: slow dmem %s bytes=%zu ms=%llu\n", what, bytes,
                (unsigned long long)((now - start) / 1000u));
}

/* CPU read-write, unless the kernel wants the GPU bits too. */
static int map_protection = CPU_READ_WRITE;

static int reserve(void *context, uintptr_t address, size_t bytes)
{
    void *at = (void *)address;
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = sceKernelReserveVirtualRange(&at, bytes, KERNEL_FIXED | KERNEL_NO_OVERWRITE, PAGE) ||
         at != (void *)address;
    slow_end(t0, "reserve", bytes);
    return rc;
}
static int allocate(void *context, size_t bytes, int64_t *offset)
{
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
#ifdef __ORBIS__
    rc = sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), bytes, PAGE, MEMORY_TYPE,
                                       offset) != 0;
#else
    rc = sceKernelAllocateMainDirectMemory(bytes, PAGE, MEMORY_TYPE, offset) != 0;
#endif
    slow_end(t0, "allocate", bytes);
    return rc;
}
static int map(void *context, uintptr_t address, size_t bytes, int64_t offset)
{
    void *at = (void *)address;
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = sceKernelMapDirectMemory(&at, bytes, map_protection, KERNEL_FIXED, offset, PAGE);
    slow_end(t0, "map", bytes);
    if (rc && map_protection == CPU_READ_WRITE) {
        /* Refused CPU-only: the GPU bits, and keep them if they work. */
        at = (void *)address;
        rc = sceKernelMapDirectMemory(&at, bytes, CPU_GPU_READ_WRITE, KERNEL_FIXED, offset, PAGE);
        if (!rc) map_protection = CPU_GPU_READ_WRITE;
    }
    return rc || at != (void *)address;
}
static int protect(void *context, uintptr_t address, size_t bytes, unsigned protection)
{
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = sceKernelMprotect((const void *)address, bytes, (int)protection) != 0;
    slow_end(t0, "protect", bytes);
    return rc;
}
static int unmap(void *context, uintptr_t address, size_t bytes)
{
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = sceKernelMunmap((void *)address, bytes) != 0;
    slow_end(t0, "unmap", bytes);
    return rc;
}
static int release(void *context, int64_t offset, size_t bytes)
{
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = sceKernelReleaseDirectMemory(offset, bytes) != 0;
    slow_end(t0, "release", bytes);
    return rc;
}

static const PwWineDmemOps ops = { NULL, PAGE, reserve, allocate, map, protect, unmap, release };
static PwWineDmem dmem;
static PwWineDmemRun runs[RUNS];
/* 0 before the self-check, 1 in use, -1 refused: every call passes. */
static int state;

static uintptr_t page_down(const void *address) { return (uintptr_t)address & ~(uintptr_t)(PAGE - 1); }
static size_t page_span(const void *address, size_t bytes)
{
    return (((uintptr_t)address + bytes + PAGE - 1) & ~(uintptr_t)(PAGE - 1)) - page_down(address);
}

/* One page through every step, inside the first region, before any use. */
static int self_check(uintptr_t address)
{
    volatile uint8_t *bytes = (volatile uint8_t *)address;
    int ok = !pw_wine_dmem_replace(&dmem, address, PAGE, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE);

    if (ok) {
        bytes[0] = 0x5a;
        bytes[PAGE - 1] = 0xa5;
        ok = !pw_wine_dmem_protect(&dmem, address, PAGE, PW_WINE_DMEM_READ | PW_WINE_DMEM_EXEC) &&
             bytes[0] == 0x5a && bytes[PAGE - 1] == 0xa5 &&
             !pw_wine_dmem_protect(&dmem, address, PAGE, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE);
    }
    return !pw_wine_dmem_replace(&dmem, address, PAGE, 0) && ok;
}

/* The Unix-side heap (pw_wine_heap) in direct memory: its spans and large
 * blocks were anonymous mappings, which the console takes from the title's
 * ~440 MiB of flexible memory, and GL drivers drained it (Slay the Spire 2:
 * 237 MiB of heap spans, then 4 fps and a crash). Each block is main direct
 * memory mapped where the kernel chooses; its offset is kept here so free can
 * release it. Called with the heap's lock held.
 *
 * The pool is shared with ntdll's committed memory and the GPU driver, which
 * can fill it (Over the Hill at 4K: Wine 6.2 GiB, the GPU 5.5). Below
 * heap_headroom of free direct memory the heap spills to flexible memory
 * instead, up to heap_spill_cap; past that it takes direct memory again.
 * Free direct memory is the largest free block as last read from the
 * kernel, less what the heap took since; it is read again once the heap has
 * grown by HEAP_RESYNC net, so allocating and freeing costs no extra call.
 * The table of blocks is itself direct memory, taken when the backing is
 * installed: in the module's .bss it would be 1.5 MiB of the flexible
 * memory this saves. */
#ifndef PW_WINE_HEAP_SLOTS
#define PW_WINE_HEAP_SLOTS (1 << 16)
#endif
enum { HEAP_SLOTS = PW_WINE_HEAP_SLOTS, HEAP_RESYNC = 16 << 20 };
typedef struct HeapBlock { uintptr_t address; size_t bytes; int64_t offset; } HeapBlock;
static HeapBlock *heap_blocks;
static unsigned heap_block_count;
static uint64_t heap_direct_bytes, heap_spilled_bytes;
static size_t heap_headroom, heap_spill_cap, heap_free_estimate;
static int64_t heap_growth = HEAP_RESYNC;  /* net, since the last reading */

static size_t heap_slot(uintptr_t address) { return (size_t)((address / PAGE) * 0x9e3779b97f4a7c15ull >> 47) & (HEAP_SLOTS - 1); }

/* The largest free block of the title's pool; nonzero if it cannot be read. */
static int largest_free_block(size_t pool, size_t *bytes)
{
    int64_t where = 0;
    *bytes = 0;
    return pool ? sceKernelAvailableDirectMemorySize(0, (int64_t)pool, PAGE, &where, bytes) : -1;
}

static void *heap_spill(size_t span)
{
    heap_spilled_bytes += span;
    return NULL;
}

static void *heap_map(void *context, size_t bytes)
{
    size_t span = (bytes + PAGE - 1) & ~(size_t)(PAGE - 1), slot;
    int64_t offset;
    void *at = NULL;
    (void)context;
    if (heap_growth >= HEAP_RESYNC) {
        /* Unreadable: no headroom, as before it was checked. */
        if (largest_free_block(sceKernelGetDirectMemorySize(), &heap_free_estimate)) heap_free_estimate = SIZE_MAX;
        heap_growth = 0;
    }
    heap_growth += (int64_t)span;
    if (heap_block_count >= HEAP_SLOTS / 2) return heap_spill(span);
    if (heap_free_estimate < span + heap_headroom && heap_spilled_bytes + span <= heap_spill_cap)
        return heap_spill(span);
    if (allocate(NULL, span, &offset)) return heap_spill(span);
    if (sceKernelMapDirectMemory(&at, span, map_protection, 0, offset, PAGE) || !at) {
        release(NULL, offset, span);
        return heap_spill(span);
    }
    for (slot = heap_slot((uintptr_t)at); heap_blocks[slot].address; slot = (slot + 1) & (HEAP_SLOTS - 1)) {}
    heap_blocks[slot] = (HeapBlock){ (uintptr_t)at, span, offset };
    heap_block_count++;
    heap_direct_bytes += span;
    if (heap_free_estimate != SIZE_MAX) heap_free_estimate = heap_free_estimate > span ? heap_free_estimate - span : 0;
    return at;
}

static int heap_unmap(void *context, void *address, size_t bytes)
{
    size_t slot, hole, next;
    (void)context;
    heap_growth -= (int64_t)bytes;
    for (slot = heap_slot((uintptr_t)address); heap_blocks[slot].address; slot = (slot + 1) & (HEAP_SLOTS - 1)) {
        HeapBlock block = heap_blocks[slot];
        if (block.address != (uintptr_t)address) continue;
        unmap(NULL, block.address, block.bytes);
        release(NULL, block.offset, block.bytes);
        heap_block_count--;
        heap_direct_bytes -= block.bytes;
        if (heap_free_estimate != SIZE_MAX) heap_free_estimate += block.bytes;
        /* Linear-probe deletion: move later entries of the run back. */
        heap_blocks[slot].address = 0;
        for (hole = slot, next = (slot + 1) & (HEAP_SLOTS - 1); heap_blocks[next].address;
             next = (next + 1) & (HEAP_SLOTS - 1)) {
            size_t home = heap_slot(heap_blocks[next].address);
            if (((next - home) & (HEAP_SLOTS - 1)) >= ((next - hole) & (HEAP_SLOTS - 1))) {
                heap_blocks[hole] = heap_blocks[next];
                heap_blocks[next].address = 0;
                hole = next;
            }
        }
        return 0;
    }
    /* Not ours: spilled, or an anonymous mapping from before direct memory. */
    heap_spilled_bytes = heap_spilled_bytes > bytes ? heap_spilled_bytes - bytes : 0;
    return 1;
}

/* WINE_PS5_HEAP_HEADROOM_MB and WINE_PS5_HEAP_SPILL_MB override the
 * defaults, 256 and 128 MiB. */
static size_t heap_megabytes(const char *name, unsigned long megabytes)
{
    const char *value = getenv(name);
    char *end;
    unsigned long set;

    if (value && *value) {
        set = strtoul(value, &end, 10);
        if (!*end) megabytes = set;
    }
    return (size_t)megabytes << 20;
}

static void heap_use_direct_memory(void)
{
    static const PwWineHeapBacking backing = { NULL, heap_map, heap_unmap };
    size_t table = ((size_t)HEAP_SLOTS * sizeof(HeapBlock) + PAGE - 1) & ~(size_t)(PAGE - 1);
    int64_t offset;
    void *at = NULL;

    if (allocate(NULL, table, &offset)) {
        fprintf(stderr, "wine-ps5: heap stays in flexible memory (no direct memory for its table)\n");
        return;
    }
    if (sceKernelMapDirectMemory(&at, table, map_protection, 0, offset, PAGE) || !at) {
        release(NULL, offset, table);
        fprintf(stderr, "wine-ps5: heap stays in flexible memory (its table could not be mapped)\n");
        return;
    }
    memset(at, 0, table);   /* direct memory is not handed out zeroed */
    heap_blocks = at;
    heap_headroom = heap_megabytes("WINE_PS5_HEAP_HEADROOM_MB", 256);
    heap_spill_cap = heap_megabytes("WINE_PS5_HEAP_SPILL_MB", 128);
    pw_wine_heap_set_backing(&backing);
    fprintf(stderr, "wine-ps5: heap in direct memory (headroom %zu MiB, spill %zu MiB)\n", heap_headroom >> 20,
            heap_spill_cap >> 20);
}

/* ntdll's reserved area [base, +size), already reserved: backed by direct
 * memory from now on. ntdll also gives each reservation it makes at a fixed
 * address outside those areas: the console does not back a bare reservation
 * when it is protected, so committing one would leave it without memory.
 * Such a region is dropped again when it is unmapped. */
void __wine_ps5_dmem_region(void *base, size_t size)
{
    if (!state && pw_wine_dmem_init(&dmem, &ops, runs, RUNS, 1)) state = -1;
    if (state < 0) return;
    if (pw_wine_dmem_add_region(&dmem, (uintptr_t)base, size, 1)) {
        fprintf(stderr, "wine-ps5: no direct memory region for %p, %zu bytes\n", base, size);
        return;
    }
    if (!state) {
        state = self_check((uintptr_t)base) ? 1 : -1;
        fprintf(stderr, "wine-ps5: direct memory %s for anonymous memory (region %p, %zu MiB)\n",
                state > 0 ? "in use" : "refused", base, size >> 20);
        if (state > 0 && !getenv("WINE_PS5_HEAP_FLEXIBLE")) heap_use_direct_memory();
    }
}

void *__wine_ps5_mmap(void *address, size_t bytes, int protection, int flags, int fd, off_t offset)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0;

    if (state <= 0 || !(flags & MAP_FIXED) || (uintptr_t)address != at ||
        (!pw_wine_dmem_split(&dmem, at, end - at, &piece) && piece == end - at))
        return mmap(address, bytes, protection, flags, fd, offset);
    if (!(flags & MAP_ANON)) {
        void *mapped;

        /* A private file view (an image section) is read into direct
         * memory instead: ntdll falls back to pread when mmap reports
         * ENODEV. On the console a fixed file mapping over a reservation
         * does not give the requested pages. */
        if (!(flags & MAP_SHARED)) {
            errno = ENODEV;
            return MAP_FAILED;
        }
        /* Shared memory stays a file mapping, over what is there, as
         * before direct memory; only the exact address is accepted. */
        mapped = mmap(address, bytes, protection, flags, fd, offset);
        if (mapped == MAP_FAILED) return MAP_FAILED;
        if (mapped != address) {
            munmap(mapped, bytes);
            errno = ENOMEM;
            return MAP_FAILED;
        }
        for (; at < end; at += piece)
            if (pw_wine_dmem_split(&dmem, at, end - at, &piece)) (void)pw_wine_dmem_adopt(&dmem, at, piece);
        return address;
    }
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece))
            status |= pw_wine_dmem_replace(&dmem, at, piece, (unsigned)protection & 7);
        else if (mmap((void *)at, piece, protection, flags, -1, 0) == MAP_FAILED)
            status = -1;
    if (!status) return address;
    errno = ENOMEM;
    return MAP_FAILED;
}

/* Owned pages give their direct memory back, then their address space. */
int __wine_ps5_munmap(void *address, size_t bytes)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0;

    if (state <= 0) return munmap(address, bytes);
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece))
            status |= pw_wine_dmem_replace(&dmem, at, piece, 0) | sceKernelMunmap((void *)at, piece);
        else
            status |= munmap((void *)at, piece);
    pw_wine_dmem_remove_regions(&dmem, page_down(address), page_span(address, bytes));
    if (status) errno = EINVAL;
    return status ? -1 : 0;
}

int __wine_ps5_mprotect(void *address, size_t bytes, int protection)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0;

    if (state <= 0) return mprotect(address, bytes, protection);
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece))
            status |= pw_wine_dmem_protect(&dmem, at, piece, (unsigned)protection & 7);
        else
            status |= mprotect((void *)at, piece, protection);
    if (status) errno = ENOMEM;
    return status ? -1 : 0;
}

/* For the title's log, as __wine_virtual_stats: direct memory backed now and
 * at most, its runs and refused calls, the heap's mapped and peak mapped
 * bytes, the direct memory backed below 4 GiB now and at most, then the
 * heap's bytes in direct memory and spilled to flexible memory. Returns how
 * many counters there are. */
unsigned int __wine_ps5_memory_stats(uint64_t *out, unsigned int count)
{
    PwWineDmemStats dmem_stats = { 0 };
    PwWineHeapStats heap_stats;
    uint64_t values[10];

    if (state > 0) pw_wine_dmem_stats(&dmem, &dmem_stats);
    pw_wine_heap_stats(&heap_stats);
    values[0] = dmem_stats.backed_bytes;
    values[1] = dmem_stats.peak_backed_bytes;
    values[2] = dmem_stats.runs;
    values[3] = dmem_stats.failures;
    values[4] = heap_stats.mapped_bytes;
    values[5] = heap_stats.peak_mapped_bytes;
    values[6] = dmem_stats.low_backed_bytes;
    values[7] = dmem_stats.peak_low_backed_bytes;
    values[8] = heap_direct_bytes;
    values[9] = heap_spilled_bytes;
    for (unsigned int i = 0; i < count && i < 10; i++) out[i] = values[i];
    return 10;
}

/* Wine patch 0603 uses the title's direct-memory pool as physical memory.
 * The largest free block estimates availability, with an optimistic 1 GiB
 * floor to keep fragmented or unavailable readings from rejecting startup.
 * If the pool query returns zero, report a 4 GiB fallback. These estimates
 * do not reserve memory; the kernel still enforces allocation limits. */
void __wine_ps5_memory_budget(unsigned long long *total, unsigned long long *available)
{
    enum { GIB = 1024 * 1024 * 1024 };
    static int logged;
    size_t pool = sceKernelGetDirectMemorySize(), free_bytes;
    int status = largest_free_block(pool, &free_bytes);

    if (status) free_bytes = 0;
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "wine-ps5: direct memory %zu MiB, largest free block %zu MiB (status %#x)\n",
                pool >> 20, free_bytes >> 20, (unsigned)status);
    *total = pool ? pool : 4ull * GIB;
    *available = free_bytes > GIB ? free_bytes : GIB;
    if (*available > *total) *available = *total;
}
