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
 * There is no hardware cursor and no idle GDI path here: the title shows GDI
 * through its own VideoOut until it hands the display over, and RADV's WSI
 * (wsi_orbis.c) opens VideoOut at swapchain creation. */
#include "pw_vulkan_radv_orbis.h"

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

        __atomic_compare_exchange_n(&stats.first_present_ns, &none, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        __atomic_store_n(&stats.last_present_ns, now, __ATOMIC_RELAXED);
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
