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
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>

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

/* WINE_PS5_TRACE_PROT=LOW-HIGH (hex addresses): every mmap, munmap and
 * mprotect touching that range is written to stderr with its protection,
 * the path (direct memory or libc) and the result (ps4-wine milestone i:
 * which call left an image page without execute). */
static uintptr_t trace_low, trace_high;
static int trace_state = -1;

static int trace_on(void)
{
    if (trace_state < 0) {
        const char *value = getenv("WINE_PS5_TRACE_PROT");
        char *rest = NULL;
        trace_state = 0;
        if (value) {
            trace_low = (uintptr_t)strtoull(value, &rest, 16);
            if (rest && *rest == '-') trace_high = (uintptr_t)strtoull(rest + 1, NULL, 16);
            trace_state = trace_high > trace_low;
        }
    }
    return trace_state > 0;
}

static int traced(uintptr_t at, uintptr_t end)
{
    return trace_on() && at < trace_high && end > trace_low;
}

/* One line, one write(2): this libc's stderr takes no lock (its own pthread_create is not
 * linked), and the watcher prints from a thread of its own. */
static void trace_print(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void trace_print(const char *format, ...)
{
    char line[320];
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0) return;
    if ((size_t)length >= sizeof(line)) {
        length = (int)sizeof(line) - 1;
        line[length - 1] = '\n';
    }
    (void)!write(2, line, (size_t)length);
}

static void trace_call(const char *what, uintptr_t at, size_t bytes, int protection, const char *path, int rc)
{
    trace_print("wine-ps5: trace %s %#lx+%#lx prot=%#x path=%s rc=%d\n", what, (unsigned long)at,
                (unsigned long)bytes, (unsigned)protection, path, rc);
}

/* The kernel's view of the traced pages (ps4-wine milestone i: who changes an image page's
 * protection after Wine set it). Every look compares each traced page (at most TRACE_PAGES) with
 * the last look and logs what changed:
 *   call   right after every mmap, munmap and mprotect that comes through here while tracing,
 *          inside the range or not (a call beside the range can move a traced page's region edge);
 *   dl-before, dl-after   around each module load (pw_wine_dl_libc.c), with the contents' hashes;
 *   watch  from a thread that looks every millisecond, labelled with the load under way or the
 *          last one ("during=", "after=").
 * trace_lock is held across each of our calls and its look, so a change the watcher reports was
 * made by none of them. A change of the region alone (the protection unchanged) is marked
 * "range-only". */
typedef struct VqInfo {
    void *start, *end;
    int64_t offset;
    int32_t protection, type;
    uint32_t kind;
    char name[32];
} VqInfo;
extern int sceKernelVirtualQuery(const void *address, int flags, VqInfo *info, size_t size);

enum { TRACE_PAGES = 8, TRACE_STACK = 256 << 10 };
typedef struct TracePage { int known, prot; uintptr_t low, high; uint64_t hash; int hashed; } TracePage;
typedef struct TraceHash { int valid; uint64_t value; } TraceHash;
static TracePage trace_pages[TRACE_PAGES];
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t trace_owner;
static int trace_held;
static char trace_loading[64];          /* the load under way or the last one, for the watcher */
static int trace_watcher;               /* 0 not started, 1 running, -1 off or refused */

/* trace_lock, unless this thread holds it already (a fault inside a look that reaches
 * __wine_ps5_mprotect through patch 1140): that call is then not traced. */
static int trace_enter(void)
{
    if (__atomic_load_n(&trace_held, __ATOMIC_ACQUIRE) && pthread_equal(trace_owner, pthread_self())) return 0;
    if (pthread_mutex_lock(&trace_lock)) return 0;
    trace_owner = pthread_self();
    __atomic_store_n(&trace_held, 1, __ATOMIC_RELEASE);
    return 1;
}
static void trace_leave(void)
{
    __atomic_store_n(&trace_held, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&trace_lock);
}

static unsigned long long trace_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000u + (unsigned long long)ts.tv_nsec / 1000u;
}

/* The protection the kernel reports for address (-1 if nothing is mapped there) and its region. */
int __wine_ps5_prot_query(const void *address, uintptr_t *low, uintptr_t *high)
{
    VqInfo info;
    memset(&info, 0, sizeof(info));
    if (sceKernelVirtualQuery(address, 0, &info, sizeof(info))) {
        if (low) *low = 0;
        if (high) *high = 0;
        return -1;
    }
    if (low) *low = (uintptr_t)info.start;
    if (high) *high = (uintptr_t)info.end;
    return info.protection;
}

static uint64_t page_hash(uintptr_t at)
{
    const uint8_t *bytes = (const uint8_t *)at;
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < PAGE; i++) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

/* The traced pages' contents, read without trace_lock: a page that faults on the read must not
 * take the lock's holder into Wine's fault path. Only pages the kernel reports readable. */
static void trace_hash_pages(TraceHash *hashes)
{
    uintptr_t at = trace_low & ~(uintptr_t)(PAGE - 1);
    for (unsigned i = 0; i < TRACE_PAGES; i++, at += PAGE) {
        int prot = at < trace_high ? __wine_ps5_prot_query((const void *)at, NULL, NULL) : -1;
        hashes[i].valid = prot > 0 && (prot & 1);
        hashes[i].value = hashes[i].valid ? page_hash(at) : 0;
    }
}

/* With trace_lock held. hashes: NULL, or the contents (mode 1 a baseline before a load, mode 2
 * compared with it after the load). Returns how many pages changed. */
static unsigned trace_look(const char *what, const char *name, const TraceHash *hashes, int mode)
{
    unsigned long long now = trace_now_us();
    uintptr_t at = trace_low & ~(uintptr_t)(PAGE - 1);
    unsigned changed = 0;

    for (unsigned i = 0; i < TRACE_PAGES && at < trace_high; i++, at += PAGE) {
        TracePage *seen = &trace_pages[i];
        uintptr_t low, high;
        int prot = __wine_ps5_prot_query((const void *)at, &low, &high), hash_changed = 0, same;
        char old[16];

        if (hashes) {
            hash_changed = mode == 2 && hashes[i].valid && seen->hashed && hashes[i].value != seen->hash;
            seen->hashed = hashes[i].valid;
            seen->hash = hashes[i].value;
        }
        same = seen->known && prot == seen->prot;
        if (same && low == seen->low && high == seen->high && !hash_changed) continue;
        if (seen->known) snprintf(old, sizeof(old), "%d", seen->prot);
        else snprintf(old, sizeof(old), "?");
        trace_print("wine-ps5: prot %s%s%s t_us=%llu page=%#lx prot=%s->%d range=%#lx-%#lx%s%s\n", what,
                    name ? " " : "", name ? name : "", now, (unsigned long)at, old, prot, (unsigned long)low,
                    (unsigned long)high, hash_changed ? " contents=changed" : "",
                    same && !hash_changed ? " range-only" : "");
        seen->known = 1;
        seen->prot = prot;
        seen->low = low;
        seen->high = high;
        changed++;
    }
    return changed;
}

static void *trace_watch(void *arg)
{
    (void)arg;
    for (;;) {
        if (trace_enter()) {
            trace_look("watch", trace_loading[0] ? trace_loading : NULL, NULL, 0);
            trace_leave();
        }
        usleep(1000);
    }
    return NULL;
}

/* With trace_lock held: the watcher starts at the first look, unless WINE_PS5_TRACE_WATCH=0. Its
 * stack is mapped only now (nothing in the module's .bss when not tracing) and set by the caller,
 * which orbis-compat passes through untouched under the default thread-stack policy (keep). It
 * starts with every signal blocked: it has no Wine thread data for a handler to use. */
static void trace_start_watcher(void)
{
    const char *value = getenv("WINE_PS5_TRACE_WATCH");
    sigset_t all, old;
    pthread_attr_t attr;
    pthread_t thread;
    void *stack;
    int rc, masked;

    if (trace_watcher) return;
    trace_watcher = -1;
    if (value && !strcmp(value, "0")) return;
    stack = mmap(NULL, TRACE_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stack == MAP_FAILED) {
        trace_print("wine-ps5: prot watcher not started (no stack, errno %d)\n", errno);
        return;
    }
    sigfillset(&all);
    masked = !pthread_sigmask(SIG_BLOCK, &all, &old);
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, stack, TRACE_STACK);
    rc = pthread_create(&thread, &attr, trace_watch, NULL);
    pthread_attr_destroy(&attr);
    if (masked) pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc) {
        munmap(stack, TRACE_STACK);
        trace_print("wine-ps5: prot watcher not started rc=%d\n", rc);
        return;
    }
    pthread_detach(thread);
    trace_watcher = 1;
    trace_print("wine-ps5: prot watcher on, %#lx-%#lx every 1 ms, signals %s\n", (unsigned long)trace_low,
                (unsigned long)trace_high, masked ? "blocked" : "not blocked");
}

/* After one of our calls (trace_lock taken by trace_enter): the kernel's view of the traced pages,
 * then the lock released. errno is the call's. */
static void trace_after_call(const char *what, const void *address, size_t bytes)
{
    int saved = errno;
    char name[64];

    snprintf(name, sizeof(name), "%s@%#lx+%#lx", what, (unsigned long)(uintptr_t)address, (unsigned long)bytes);
    trace_start_watcher();
    trace_look("call", name, NULL, 0);
    trace_leave();
    errno = saved;
}

/* Around a module load (pw_wine_dl_libc.c): the traced pages before and after, with their contents'
 * hashes; every bracket logs one line even when nothing changed. */
void __wine_ps5_prot_snapshot(const char *phase, const char *module)
{
    TraceHash hashes[TRACE_PAGES];
    const char *base;
    unsigned changed;
    int before;

    if (!trace_on()) return;
    base = module ? strrchr(module, '/') : NULL;
    base = base ? base + 1 : module ? module : "?";
    before = !strcmp(phase, "dl-before");
    trace_hash_pages(hashes);
    if (!trace_enter()) return;
    trace_start_watcher();
    snprintf(trace_loading, sizeof(trace_loading), "%s=%s", before ? "during" : "after", base);
    changed = trace_look(phase, base, hashes, before ? 1 : 2);
    trace_print("wine-ps5: prot %s %s t_us=%llu changed=%u\n", phase, base, trace_now_us(), changed);
    trace_leave();
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

static void *mmap_body(void *address, size_t bytes, int protection, int flags, int fd, off_t offset)
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
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece)) {
            int rc = pw_wine_dmem_replace(&dmem, at, piece, (unsigned)protection & 7);
            if (traced(at, at + piece)) trace_call("mmap", at, piece, protection, "dmem", rc);
            status |= rc;
        } else {
            int rc = mmap((void *)at, piece, protection, flags, -1, 0) == MAP_FAILED ? -1 : 0;
            if (traced(at, at + piece)) trace_call("mmap", at, piece, protection, "libc", rc);
            if (rc) status = -1;
        }
    if (!status) return address;
    errno = ENOMEM;
    return MAP_FAILED;
}

/* Owned pages give their direct memory back, then their address space. */
static int munmap_body(void *address, size_t bytes)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0;

    if (state <= 0) return munmap(address, bytes);
    for (; at < end; at += piece) {
        int owned = pw_wine_dmem_split(&dmem, at, end - at, &piece), rc;
        if (owned) rc = pw_wine_dmem_replace(&dmem, at, piece, 0) | sceKernelMunmap((void *)at, piece);
        else rc = munmap((void *)at, piece);
        if (traced(at, at + piece)) trace_call("munmap", at, piece, 0, owned ? "dmem" : "libc", rc);
        status |= rc;
    }
    pw_wine_dmem_remove_regions(&dmem, page_down(address), page_span(address, bytes));
    if (status) errno = EINVAL;
    return status ? -1 : 0;
}

static int mprotect_body(void *address, size_t bytes, int protection)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0;

    if (state <= 0) return mprotect(address, bytes, protection);
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece)) {
            int rc = pw_wine_dmem_protect(&dmem, at, piece, (unsigned)protection & 7);
            if (traced(at, at + piece)) trace_call("mprotect", at, piece, protection, "dmem", rc);
            status |= rc;
        } else {
            int rc = mprotect((void *)at, piece, protection);
            if (traced(at, at + piece)) trace_call("mprotect", at, piece, protection, "libc", rc);
            status |= rc;
        }
    if (status) errno = ENOMEM;
    return status ? -1 : 0;
}

/* While tracing, each call holds trace_lock with its look (trace_after_call). */
void *__wine_ps5_mmap(void *address, size_t bytes, int protection, int flags, int fd, off_t offset)
{
    void *result;
    int held;

    if (!trace_on()) return mmap_body(address, bytes, protection, flags, fd, offset);
    held = trace_enter();
    result = mmap_body(address, bytes, protection, flags, fd, offset);
    if (held) trace_after_call("mmap", address, bytes);
    return result;
}

int __wine_ps5_munmap(void *address, size_t bytes)
{
    int result, held;

    if (!trace_on()) return munmap_body(address, bytes);
    held = trace_enter();
    result = munmap_body(address, bytes);
    if (held) trace_after_call("munmap", address, bytes);
    return result;
}

int __wine_ps5_mprotect(void *address, size_t bytes, int protection)
{
    int result, held;

    if (!trace_on()) return mprotect_body(address, bytes, protection);
    held = trace_enter();
    result = mprotect_body(address, bytes, protection);
    if (held) trace_after_call("mprotect", address, bytes);
    return result;
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
