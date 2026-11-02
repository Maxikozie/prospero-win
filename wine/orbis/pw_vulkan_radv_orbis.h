/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VULKAN_RADV_ORBIS_H
#define PW_VULKAN_RADV_ORBIS_H

#include <stddef.h>
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

/* The counters of the pthread layer linked into libvulkan.prx (its own copy,
 * which sees only the threads RADV creates): threads created and raised, and
 * the floor in bytes, 0 while no thread has resolved it (it is read without
 * resolving it: resolving would take the reader's own stack). Any pointer
 * may be NULL. */
void pw_vulkan_thread_stats(unsigned long *created, unsigned long *raised, size_t *floor_bytes);

/* Placed CPU aliases (VK_EXT_map_memory_placed, 32-bit Wine's host-visible
 * memory below 4 GiB; ps4-wine patches/mesa-ps4/0003): made so far, live now
 * and their bytes, the peak of those bytes, and refusals. Milestone g's g1m and
 * g1 lines read them (map32). Any pointer may be NULL. */
void pw_vulkan_alias_stats(uint32_t *made, uint32_t *live, uint64_t *live_bytes, uint64_t *peak_bytes,
                           uint32_t *failed);

/* The interval between consecutive counted presents, in 1 ms buckets
 * (bucket i: i ms <= interval < i+1 ms; the last bucket holds everything
 * from PW_VULKAN_HIST_BUCKETS-1 ms up), cumulative, and the longest interval
 * since the last reset. Copies min(count, PW_VULKAN_HIST_BUCKETS) buckets;
 * with reset_max the longest interval starts again from 0 (milestone g's
 * window starts at a mark). Returns the number of buckets copied. */
enum { PW_VULKAN_HIST_BUCKETS = 251 };
size_t pw_vulkan_present_hist(uint64_t *buckets, size_t count, uint64_t *max_ns, int reset_max);

/* VideoOut's hardware cursor over the swapchain's frames: win32u hands the
 * application's cursor to pw_videoout_cursor after each present (patch
 * 0740); argb is its width x height image in straight-alpha ARGB, cut to
 * 64x64, with its top left corner at (x, y) of a space_width x space_height
 * desktop; NULL hides it. 0 shown or hidden, 1 no swapchain's VideoOut open,
 * <0 failed (pw_videoout_cursor_orbis.h). */
int pw_videoout_cursor(const uint32_t *argb, uint32_t width, uint32_t height, int32_t x, int32_t y,
                       uint32_t space_width, uint32_t space_height);

/* Its counters, cumulative, for the title's lines. */
struct pw_vulkan_cursor_stats
{
    uint32_t size;          /* sizeof(struct pw_vulkan_cursor_stats), set by the caller */
    uint32_t output;        /* 1 while a swapchain's VideoOut is open */
    uint64_t calls;
    uint64_t shows;         /* returned 0 with an image */
    uint64_t updates;       /* images given to VideoOut (enable or a new image address) */
    uint64_t hides;         /* returned 0 with none */
    uint64_t noout;         /* returned 1 */
    uint64_t errors;        /* returned < 0 */
    int32_t last_rc;        /* the last failing VideoOut status (-1: no cursor memory), 0 if none */
    uint32_t x, y;          /* the last position given to VideoOut */
    uint32_t format;        /* the conversion applied (PW_CURSOR_ORBIS_SWAP_RB | _PREMULTIPLY) */
};

/* Fills *stats up to stats->size bytes; 0, or -1 for a NULL or too-small buffer. */
int pw_vulkan_cursor_stats(struct pw_vulkan_cursor_stats *stats);

#endif
