/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* libvulkan.prx on the PS4: mesa-ps4's RADV, linked --whole-archive by
 * tools/build_wine_orbis.sh, behind the two entry points win32u looks up in
 * libvulkan.so (Mesa's archive exports only the loader's ICD interface: the
 * instance lookup resolves every entry point, the device lookup is the
 * runtime's own), as wine/ps5/pw_vulkan_radv.c does on the PS5.
 *
 * The device lookup hands out counting wrappers for swapchain creation,
 * presents and submits, so the title's milestone lines take frames, the
 * swapchain size and lost-device evidence from what reached the driver
 * (pw_vulkan_stats), whichever program presents. Each wrapper calls the
 * driver's own entry point, which RADV resolves the same for every device.
 *
 * win32u shows the application's cursor over the swapchain's frames with
 * VideoOut's hardware cursor (pw_videoout_cursor), as on the PS5. There is
 * no idle GDI path here: the title shows GDI through its own VideoOut until
 * it hands the display over, and RADV's WSI (wsi_orbis.c) opens VideoOut at
 * swapchain creation and closes it with the swapchain. */
#include "pw_vulkan_radv_orbis.h"
#include "pw_videoout_cursor_orbis.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
PFN_vkVoidFunction VKAPI_CALL vk_common_GetDeviceProcAddr(VkDevice device, const char *name);

/* RADV's C++ objects register their destructors with
 * __cxa_atexit(..., &__dso_handle), which an executable gets from crt1.o;
 * a PRX links no crt. */
#ifdef __ORBIS__
__attribute__((visibility("hidden"))) void *__dso_handle = &__dso_handle;

/* Mesa names the process (util/u_process.c: program_invocation_name, for
 * driconf and the instance) and strlen()s it. In a PRX that is this
 * module's own musl __progname_full, which only crt startup sets: NULL,
 * and the first vkCreateInstance faulted in strlen (milestone e1, launch
 * 1). module_start runs this before anything else of RADV. */
extern char *__progname, *__progname_full;
__attribute__((constructor)) static void name_process(void)
{
    static char name[] = "wine";
    if (!__progname_full) __progname_full = name;
    if (!__progname) __progname = name;
}
#endif

static struct pw_vulkan_stats stats;
/* Present intervals (milestone g): 1 ms buckets and the longest since a reset. */
static uint64_t present_hist[PW_VULKAN_HIST_BUCKETS], present_max_ns;
static PFN_vkCreateSwapchainKHR real_create_swapchain;
static PFN_vkQueuePresentKHR real_queue_present;
static PFN_vkQueueSubmit real_queue_submit;
static PFN_vkQueueSubmit2 real_queue_submit2;

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void count_error(VkResult result)
{
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) return;
    __atomic_store_n(&stats.last_error, (int32_t)result, __ATOMIC_RELAXED);
    if (result == VK_ERROR_DEVICE_LOST) __atomic_add_fetch(&stats.device_lost, 1, __ATOMIC_RELAXED);
}

static VKAPI_ATTR VkResult VKAPI_CALL count_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                                             const VkAllocationCallbacks *allocator,
                                                             VkSwapchainKHR *swapchain)
{
    VkResult result = real_create_swapchain(device, info, allocator, swapchain);

    __atomic_store_n(&stats.swapchain_result, (int32_t)result, __ATOMIC_RELAXED);
    if (result == VK_SUCCESS && info)
    {
        __atomic_store_n(&stats.width, info->imageExtent.width, __ATOMIC_RELAXED);
        __atomic_store_n(&stats.height, info->imageExtent.height, __ATOMIC_RELAXED);
        __atomic_store_n(&stats.format, (uint32_t)info->imageFormat, __ATOMIC_RELAXED);
        __atomic_add_fetch(&stats.swapchains, 1, __ATOMIC_RELEASE);
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL count_queue_present(VkQueue queue, const VkPresentInfoKHR *info)
{
    VkResult result = real_queue_present(queue, info);

    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
    {
        uint64_t now = now_ns(), none = 0;
        uint64_t last = __atomic_exchange_n(&stats.last_present_ns, now, __ATOMIC_RELAXED);

        __atomic_compare_exchange_n(&stats.first_present_ns, &none, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        if (last && now > last)
        {
            uint64_t interval = now - last, ms = interval / 1000000u, seen;

            __atomic_add_fetch(&present_hist[ms < PW_VULKAN_HIST_BUCKETS ? ms : PW_VULKAN_HIST_BUCKETS - 1], 1,
                               __ATOMIC_RELAXED);
            seen = __atomic_load_n(&present_max_ns, __ATOMIC_RELAXED);
            while (interval > seen &&
                   !__atomic_compare_exchange_n(&present_max_ns, &seen, interval, 1, __ATOMIC_RELAXED,
                                                __ATOMIC_RELAXED))
                ;
        }
        __atomic_add_fetch(&stats.presents, 1, __ATOMIC_RELEASE);
    }
    else
    {
        __atomic_add_fetch(&stats.present_failures, 1, __ATOMIC_RELAXED);
        count_error(result);
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL count_queue_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits,
                                                         VkFence fence)
{
    VkResult result = real_queue_submit(queue, count, submits, fence);

    __atomic_add_fetch(&stats.submits, 1, __ATOMIC_RELAXED);
    count_error(result);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL count_queue_submit2(VkQueue queue, uint32_t count,
                                                          const VkSubmitInfo2 *submits, VkFence fence)
{
    VkResult result = real_queue_submit2(queue, count, submits, fence);

    __atomic_add_fetch(&stats.submits, 1, __ATOMIC_RELAXED);
    count_error(result);
    return result;
}

/* The counting wrapper for NAME, keeping the driver's entry point REAL;
 * REAL itself when NAME is not counted or the driver has none. */
static PFN_vkVoidFunction counted(const char *name, PFN_vkVoidFunction real)
{
    static const struct
    {
        const char *name;
        PFN_vkVoidFunction *real;
        PFN_vkVoidFunction wrapper;
    } table[] = {
        { "vkCreateSwapchainKHR", (PFN_vkVoidFunction *)&real_create_swapchain, (PFN_vkVoidFunction)count_create_swapchain },
        { "vkQueuePresentKHR", (PFN_vkVoidFunction *)&real_queue_present, (PFN_vkVoidFunction)count_queue_present },
        { "vkQueueSubmit", (PFN_vkVoidFunction *)&real_queue_submit, (PFN_vkVoidFunction)count_queue_submit },
        { "vkQueueSubmit2", (PFN_vkVoidFunction *)&real_queue_submit2, (PFN_vkVoidFunction)count_queue_submit2 },
        { "vkQueueSubmit2KHR", (PFN_vkVoidFunction *)&real_queue_submit2, (PFN_vkVoidFunction)count_queue_submit2 },
    };
    size_t i;

    if (!name || !real) return real;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    {
        PFN_vkVoidFunction none = NULL;

        if (strcmp(name, table[i].name)) continue;
        __atomic_compare_exchange_n(table[i].real, &none, real, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        return table[i].wrapper;
    }
    return real;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    return counted(name, vk_common_GetDeviceProcAddr(device, name));
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (name && !strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    return counted(name, vk_icdGetInstanceProcAddr(instance, name));
}

int pw_vulkan_stats(struct pw_vulkan_stats *out)
{
    struct pw_vulkan_stats now;
    uint32_t size;

    if (!out || out->size < sizeof(out->size)) return -1;
    size = out->size < sizeof(now) ? out->size : (uint32_t)sizeof(now);
    now.size = (uint32_t)sizeof(now);
    now.swapchains = __atomic_load_n(&stats.swapchains, __ATOMIC_ACQUIRE);
    now.width = __atomic_load_n(&stats.width, __ATOMIC_RELAXED);
    now.height = __atomic_load_n(&stats.height, __ATOMIC_RELAXED);
    now.format = __atomic_load_n(&stats.format, __ATOMIC_RELAXED);
    now.swapchain_result = __atomic_load_n(&stats.swapchain_result, __ATOMIC_RELAXED);
    now.presents = __atomic_load_n(&stats.presents, __ATOMIC_ACQUIRE);
    now.present_failures = __atomic_load_n(&stats.present_failures, __ATOMIC_RELAXED);
    now.last_error = __atomic_load_n(&stats.last_error, __ATOMIC_RELAXED);
    now.device_lost = __atomic_load_n(&stats.device_lost, __ATOMIC_RELAXED);
    now.first_present_ns = __atomic_load_n(&stats.first_present_ns, __ATOMIC_RELAXED);
    now.last_present_ns = __atomic_load_n(&stats.last_present_ns, __ATOMIC_RELAXED);
    now.submits = __atomic_load_n(&stats.submits, __ATOMIC_RELAXED);
    memcpy(out, &now, size);
    out->size = size;
    return 0;
}

/* The layer resolves its floor once, from the stack of the first thread that
 * creates a thread through it; with Wine's threads on their own 1 MiB kernel
 * stacks that thread can be one of them, and RADV's threads then get 1 MiB. */
extern void pw_orbis_thread_counts(unsigned long *created, unsigned long *raised)
    __asm__("_ZN5orbis12threadCountsEPmS0_");
extern size_t pw_orbis_thread_stack_floor(void) __asm__("_ZN5orbis20threadStackFloorPeekEv");

void pw_vulkan_thread_stats(unsigned long *created, unsigned long *raised, size_t *floor_bytes)
{
    pw_orbis_thread_counts(created, raised);
    if (floor_bytes) *floor_bytes = pw_orbis_thread_stack_floor();   /* read without resolving it */
}

/* ps4-wine patches/mesa-ps4/0003, in RADV's Orbis winsys (ac_orbis_drm.c). */
extern void ac_orbis_drm_alias_stats(uint32_t *made, uint32_t *live, uint64_t *live_bytes, uint64_t *peak_bytes,
                                     uint32_t *failed);

void pw_vulkan_alias_stats(uint32_t *made, uint32_t *live, uint64_t *live_bytes, uint64_t *peak_bytes,
                           uint32_t *failed)
{
    uint32_t m, l, f;
    uint64_t lb, pb;

    ac_orbis_drm_alias_stats(&m, &l, &lb, &pb, &f);
    if (made) *made = m;
    if (live) *live = l;
    if (live_bytes) *live_bytes = lb;
    if (peak_bytes) *peak_bytes = pb;
    if (failed) *failed = f;
}

size_t pw_vulkan_present_hist(uint64_t *buckets, size_t count, uint64_t *max_ns, int reset_max)
{
    size_t n = count < PW_VULKAN_HIST_BUCKETS ? count : PW_VULKAN_HIST_BUCKETS;

    for (size_t i = 0; buckets && i < n; i++) buckets[i] = __atomic_load_n(&present_hist[i], __ATOMIC_RELAXED);
    if (max_ns) *max_ns = reset_max ? __atomic_exchange_n(&present_max_ns, 0, __ATOMIC_RELAXED)
                                    : __atomic_load_n(&present_max_ns, __ATOMIC_RELAXED);
    else if (reset_max) __atomic_store_n(&present_max_ns, 0, __ATOMIC_RELAXED);
    return buckets ? n : 0;
}

/* --- the hardware cursor -------------------------------------------------
 * wine/ps5/pw_vulkan_radv.c's, on the PS4: VideoOut composes the image over
 * the swapchain's frames at a position in the shown frame's pixels. RADV's
 * WSI opens VideoOut per swapchain and closes it with the swapchain (port
 * 255 opens once at a time), so both calls are wrapped (tools/
 * build_wine_orbis.sh: --wrap) and the cursor follows the open handle; its
 * state goes with a closed one. The title's gc1 probe measured the PS4's
 * plane over HDMI (2026-11-02): it reads win32u's straight-alpha ARGB words
 * as the PS5's does (red stays red, alpha 0 shows the frame, half alpha
 * blends straight), at frame pixels 1:1, clipped at the frame's edge, and a
 * closed port takes the cursor with it. So PW_CURSOR_ORBIS_FORMAT, the
 * conversion, is none. */
#ifndef PW_CURSOR_ORBIS_FORMAT
#define PW_CURSOR_ORBIS_FORMAT 0
#endif

extern int __real_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter);
extern int __real_sceVideoOutClose(int32_t handle);
extern int sceVideoOutCursorEnable(int32_t handle, int32_t index, const void *address);
extern int sceVideoOutCursorDisable(int32_t handle, int32_t index);
extern int sceVideoOutCursorSetImageAddress(int32_t handle, int32_t index, const void *address);
extern int sceVideoOutCursorSetPosition(int32_t handle, int32_t index, uint32_t x, uint32_t y);
extern size_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t bytes, size_t alignment, int type,
                                         int64_t *offset);
extern int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags, int64_t offset,
                                    size_t alignment);

/* Two images in display-readable direct memory (write-combined Garlic,
 * CPU and GPU read-write), written alternately so the one on screen never
 * changes under the display. */
enum { CURSOR_MEMORY = 64 << 10, CURSOR_GARLIC = 3, CURSOR_PROTECTION = 0x33 };

static struct {
    int lock;
    int handle;
    uint32_t *images;
    int failed, enabled, current;
    uint32_t shown[PW_CURSOR_ORBIS_PIXELS];
    struct pw_vulkan_cursor_stats stats;
} cursor = { .handle = -1 };

static void cursor_lock(void)
{
    while (__atomic_exchange_n(&cursor.lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
}

static void cursor_unlock(void)
{
    __atomic_store_n(&cursor.lock, 0, __ATOMIC_RELEASE);
}

int __wrap_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter);
int __wrap_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter)
{
    int handle = __real_sceVideoOutOpen(user, bus, index, parameter);

    cursor_lock();
    pw_cursor_orbis_opened(&cursor.handle, handle);
    if (handle >= 0) cursor.enabled = 0;   /* a new output shows no cursor until one is given */
    cursor_unlock();
    return handle;
}

int __wrap_sceVideoOutClose(int32_t handle);
int __wrap_sceVideoOutClose(int32_t handle)
{
    cursor_lock();
    if (pw_cursor_orbis_closed(&cursor.handle, handle)) cursor.enabled = 0;
    cursor_unlock();
    return __real_sceVideoOutClose(handle);
}

/* Under the lock: the images' memory, once. */
static int cursor_setup(void)
{
    int64_t offset;
    void *address = NULL;

    if (cursor.images) return 0;
    if (cursor.failed) return -1;
    if (sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), CURSOR_MEMORY, CURSOR_MEMORY,
                                      CURSOR_GARLIC, &offset) < 0 ||
        sceKernelMapDirectMemory(&address, CURSOR_MEMORY, CURSOR_PROTECTION, 0, offset, CURSOR_MEMORY) < 0) {
        cursor.failed = 1;
        fprintf(stderr, "pw_videoout_cursor: no display memory for the cursor\n");
        return -1;
    }
    cursor.images = address;
    return 0;
}

static int cursor_count(int rc, int shown)
{
    if (rc == 0 && shown) cursor.stats.shows++;
    else if (rc == 0) cursor.stats.hides++;
    else if (rc == 1) cursor.stats.noout++;
    else cursor.stats.errors++;
    return rc;
}

int pw_videoout_cursor(const uint32_t *argb, uint32_t width, uint32_t height, int32_t x, int32_t y,
                       uint32_t space_width, uint32_t space_height)
{
    uint32_t image[PW_CURSOR_ORBIS_PIXELS], fx, fy;
    int rc = 0, handle;

    if (argb && width && height && space_width && space_height) {
        pw_cursor_orbis_cut(image, argb, width, height);
        pw_cursor_orbis_convert(image, PW_CURSOR_ORBIS_FORMAT);
    }
    cursor_lock();
    cursor.stats.calls++;
    handle = cursor.handle;
    if (handle < 0) {
        cursor_count(1, 0);
        cursor_unlock();
        return 1;
    }
    if (!argb || !width || !height || !space_width || !space_height) {
        if (cursor.enabled && (rc = sceVideoOutCursorDisable(handle, 0))) cursor.stats.last_rc = rc;
        cursor.enabled = 0;
        cursor_count(rc ? -2 : 0, 0);
        cursor_unlock();
        return rc ? -2 : 0;
    }
    if (cursor_setup()) {
        cursor.stats.last_rc = -1;
        cursor_count(-1, 1);
        cursor_unlock();
        return -1;
    }
    if (!cursor.enabled || memcmp(image, cursor.shown, sizeof(image))) {
        uint32_t *next = cursor.images + (cursor.enabled ? (cursor.current ^ 1) : cursor.current) * PW_CURSOR_ORBIS_PIXELS;

        memcpy(next, image, sizeof(image));
        memcpy(cursor.shown, image, sizeof(image));
        /* The display reads memory, not the CPU's cache (write-combined
         * Garlic: the flush drains the CPU's buffers). */
        for (size_t at = 0; at < sizeof(image); at += 64) __builtin_ia32_clflush((const uint8_t *)next + at);
        __builtin_ia32_mfence();
        rc = cursor.enabled ? sceVideoOutCursorSetImageAddress(handle, 0, next)
                            : sceVideoOutCursorEnable(handle, 0, next);
        if (rc == 0) {
            cursor.current = (int)((next - cursor.images) / PW_CURSOR_ORBIS_PIXELS);
            cursor.enabled = 1;
            cursor.stats.updates++;
        } else {
            cursor.stats.last_rc = rc;
        }
    }
    /* The swapchain is shown 1:1 at its own size; win32u maps the desktop
     * onto it from the top left. */
    if (cursor.enabled) {
        pw_cursor_orbis_place(x, y, space_width, space_height, stats.width, stats.height, &fx, &fy);
        if ((rc = sceVideoOutCursorSetPosition(handle, 0, fx, fy))) cursor.stats.last_rc = rc;
        cursor.stats.x = fx;
        cursor.stats.y = fy;
    }
    rc = rc == 0 && cursor.enabled ? 0 : -2;
    cursor_count(rc, 1);
    cursor_unlock();
    return rc;
}

int pw_vulkan_cursor_stats(struct pw_vulkan_cursor_stats *out)
{
    struct pw_vulkan_cursor_stats now;
    uint32_t size;

    if (!out || out->size < sizeof(out->size)) return -1;
    size = out->size < sizeof(now) ? out->size : (uint32_t)sizeof(now);
    cursor_lock();
    now = cursor.stats;
    now.output = cursor.handle >= 0;
    cursor_unlock();
    now.size = (uint32_t)sizeof(now);
    now.format = PW_CURSOR_ORBIS_FORMAT;
    memcpy(out, &now, size);
    out->size = size;
    return 0;
}
