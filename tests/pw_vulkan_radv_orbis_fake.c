/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* wine/orbis/pw_vulkan_radv_orbis.c against a fake driver: the instance
 * lookup hands out the device lookup itself, both lookups hand out the
 * counting wrappers for the four counted entry points and the driver's own
 * pointer for the rest, the wrappers call through to the driver, and
 * pw_vulkan_stats reports the last swapchain's size and format, successful
 * presents (and the time of the first and last), failed ones with their
 * result, lost devices from presents and submits, and refuses a NULL or
 * too-small buffer while honouring a shorter one. */
#include "pw_vulkan_radv_orbis.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

static VkResult present_result = VK_SUCCESS, submit_result = VK_SUCCESS;
static int driver_calls;

static VKAPI_ATTR VkResult VKAPI_CALL driver_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                                              const VkAllocationCallbacks *allocator,
                                                              VkSwapchainKHR *swapchain)
{
    (void)device; (void)info; (void)allocator;
    driver_calls++;
    *swapchain = (VkSwapchainKHR)(uintptr_t)0x1234;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL driver_queue_present(VkQueue queue, const VkPresentInfoKHR *info)
{
    (void)queue; (void)info;
    driver_calls++;
    return present_result;
}

static VKAPI_ATTR VkResult VKAPI_CALL driver_queue_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits,
                                                          VkFence fence)
{
    (void)queue; (void)count; (void)submits; (void)fence;
    driver_calls++;
    return submit_result;
}

static VKAPI_ATTR VkResult VKAPI_CALL driver_queue_submit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits,
                                                           VkFence fence)
{
    (void)queue; (void)count; (void)submits; (void)fence;
    driver_calls++;
    return submit_result;
}

static VKAPI_ATTR void VKAPI_CALL driver_other(void)
{
}

static PFN_vkVoidFunction driver_lookup(const char *name)
{
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)driver_create_swapchain;
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)driver_queue_present;
    if (!strcmp(name, "vkQueueSubmit")) return (PFN_vkVoidFunction)driver_queue_submit;
    if (!strcmp(name, "vkQueueSubmit2")) return (PFN_vkVoidFunction)driver_queue_submit2;
    if (!strcmp(name, "vkCmdDraw")) return (PFN_vkVoidFunction)driver_other;
    return NULL;
}

PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name)
{
    (void)instance;
    return driver_lookup(name);
}

PFN_vkVoidFunction VKAPI_CALL vk_common_GetDeviceProcAddr(VkDevice device, const char *name)
{
    (void)device;
    return driver_lookup(name);
}

int main(void)
{
    PFN_vkGetDeviceProcAddr device_lookup;
    PFN_vkCreateSwapchainKHR create;
    PFN_vkQueuePresentKHR present;
    PFN_vkQueueSubmit submit;
    PFN_vkQueueSubmit2 submit2;
    VkSwapchainCreateInfoKHR info = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkSwapchainKHR swapchain;
    struct pw_vulkan_stats stats;

    device_lookup = (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetDeviceProcAddr");
    assert(device_lookup == vkGetDeviceProcAddr);
    assert(vkGetDeviceProcAddr(VK_NULL_HANDLE, "vkCmdDraw") == (PFN_vkVoidFunction)driver_other);
    assert(!vkGetDeviceProcAddr(VK_NULL_HANDLE, "vkNotAFunction"));
    create = (PFN_vkCreateSwapchainKHR)vkGetDeviceProcAddr(VK_NULL_HANDLE, "vkCreateSwapchainKHR");
    present = (PFN_vkQueuePresentKHR)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkQueuePresentKHR");
    submit = (PFN_vkQueueSubmit)vkGetDeviceProcAddr(VK_NULL_HANDLE, "vkQueueSubmit");
    submit2 = (PFN_vkQueueSubmit2)vkGetDeviceProcAddr(VK_NULL_HANDLE, "vkQueueSubmit2KHR");
    assert(create && create != driver_create_swapchain);
    assert(present && present != driver_queue_present);
    assert(submit && submit != driver_queue_submit);
    assert(!submit2);  /* the fake driver has no KHR alias: nothing to wrap */
    submit2 = (PFN_vkQueueSubmit2)vkGetDeviceProcAddr(VK_NULL_HANDLE, "vkQueueSubmit2");
    assert(submit2 && submit2 != driver_queue_submit2);

    assert(pw_vulkan_stats(NULL) == -1);
    stats.size = 2;
    assert(pw_vulkan_stats(&stats) == -1);
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    assert(!pw_vulkan_stats(&stats) && stats.size == sizeof(stats) && !stats.presents && !stats.swapchains);

    info.imageExtent.width = 1920;
    info.imageExtent.height = 1080;
    info.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
    assert(create(VK_NULL_HANDLE, &info, NULL, &swapchain) == VK_SUCCESS && swapchain);
    for (int i = 0; i < 300; i++) assert(present(VK_NULL_HANDLE, NULL) == VK_SUCCESS);
    present_result = VK_SUBOPTIMAL_KHR;
    assert(present(VK_NULL_HANDLE, NULL) == VK_SUBOPTIMAL_KHR);
    present_result = VK_ERROR_OUT_OF_DATE_KHR;
    assert(present(VK_NULL_HANDLE, NULL) == VK_ERROR_OUT_OF_DATE_KHR);
    assert(submit(VK_NULL_HANDLE, 0, NULL, VK_NULL_HANDLE) == VK_SUCCESS);
    submit_result = VK_ERROR_DEVICE_LOST;
    assert(submit2(VK_NULL_HANDLE, 0, NULL, VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST);
    assert(driver_calls == 1 + 300 + 1 + 1 + 1 + 1);

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    assert(!pw_vulkan_stats(&stats));
    assert(stats.swapchains == 1 && stats.width == 1920 && stats.height == 1080);
    assert(stats.format == VK_FORMAT_R8G8B8A8_UNORM && stats.swapchain_result == VK_SUCCESS);
    assert(stats.presents == 301 && stats.present_failures == 1);
    assert(stats.first_present_ns && stats.last_present_ns >= stats.first_present_ns);
    assert(stats.submits == 2 && stats.device_lost == 1 && stats.last_error == VK_ERROR_DEVICE_LOST);

    /* An older, shorter caller gets its prefix. */
    memset(&stats, 0xff, sizeof(stats));
    stats.size = 12;
    assert(!pw_vulkan_stats(&stats) && stats.size == 12 && stats.swapchains == 1 && stats.width == 1920);
    assert(stats.height == 0xffffffffu);
    puts("PASS");
    return 0;
}
