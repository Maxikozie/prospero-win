/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VULKAN_RADV_ORBIS_H
#define PW_VULKAN_RADV_ORBIS_H

#include <stdint.h>

/* What reached the driver, whichever program presents (libvulkan.prx's
 * pw_vulkan_stats; the title's milestone lines take frames, the swapchain
 * size and GPU-hang evidence from here, not from the program's own word). */
struct pw_vulkan_stats
{
    uint32_t size;              /* sizeof(struct pw_vulkan_stats), set by the caller */
    uint32_t swapchains;        /* vkCreateSwapchainKHR calls that succeeded */
    uint32_t width, height;     /* the last swapchain's imageExtent */
    uint32_t format;            /* and imageFormat */
    int32_t swapchain_result;   /* the last vkCreateSwapchainKHR result */
    uint64_t presents;          /* vkQueuePresentKHR calls returning VK_SUCCESS or VK_SUBOPTIMAL_KHR */
    uint64_t present_failures;  /* the others */
    int32_t last_error;         /* the last result other than those two from a present or a submit, 0 if none */
    uint32_t device_lost;       /* VK_ERROR_DEVICE_LOST from a present or a submit */
    uint64_t first_present_ns;  /* CLOCK_MONOTONIC of the first and the last counted present */
    uint64_t last_present_ns;
    uint64_t submits;           /* vkQueueSubmit and vkQueueSubmit2 calls */
};

/* Fills *stats up to stats->size bytes; returns 0, or -1 for a NULL or
 * too-small buffer. */
int pw_vulkan_stats(struct pw_vulkan_stats *stats);

#endif
