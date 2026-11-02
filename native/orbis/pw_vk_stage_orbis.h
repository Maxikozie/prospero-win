/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The PS4 port's milestone e1 in the title: RADV's libvulkan.prx opened the
 * way win32u opens it (ntdll's dlopen of "libvulkan.so": pw_wine_dl loads
 * the PRX beside ntdll, runs module_start, replays ntdll's environment into
 * it), then driven by the title itself, with no Wine program: the device
 * and the features DXVK 2.6 needs, and a headless surface whose 1920x1080
 * swapchain RADV's WSI (wsi_orbis.c) scans out, cleared to a known colour
 * with a marker square, after the title has handed VideoOut over.
 *
 * Each call fills one stage's "key=value ..." fields. A stage of a resumed
 * launch is a new process, so every stage opens what it needs.
 */
#ifndef PW_VK_STAGE_ORBIS_H
#define PW_VK_STAGE_ORBIS_H

#include <stddef.h>
#include <stdint.h>

#include "pw_vulkan_radv_orbis.h"

typedef struct PwVkHost {
    void *(*dlopen)(const char *path, int mode);     /* ntdll's: pw_wine_dl */
    void *(*dlsym)(void *handle, const char *name);
    void (*step)(const char *line);                  /* one progress record */
    uint64_t (*now_ns)(void);
} PwVkHost;

/* libvulkan through host->dlopen; 0, or -1 with the reason in why. */
int pw_vk_open(const PwVkHost *host, char *why, size_t size);
/* vk_caps: instance, device, DXVK's features, heaps, direct memory around
 * the winsys' arena. */
void pw_vk_caps(char *fields, size_t size);
/* vk_present: device, headless surface, the 1920x1080 swapchain, the
 * frames, then a second swapchain (VideoOut closed and opened again, as
 * DXVK's swapchain recreation does). shot names the file RADV's
 * ORBIS_WSI_SHOT writes; fb=ok when it holds a whole frame. */
void pw_vk_present(const char *shot, char *fields, size_t size);
/* vk_present with frames of its own (gc2: the hardware cursor over RADV's
 * frames): first and reopen frames per swapchain (0: e1's), frame called
 * after each present (swapchain 0 or 1, the frame's index), between with no
 * swapchain (the first destroyed, VideoOut closed, the second not made). */
typedef struct PwVkFrames {
    unsigned first, reopen;
    void (*frame)(void *context, unsigned swapchain, unsigned index);
    void (*between)(void *context);
    void *context;
} PwVkFrames;
void pw_vk_present_frames(const char *shot, const PwVkFrames *frames, char *fields, size_t size);
/* An export of the libvulkan.prx pw_vk_open opened (as win32u's dlsym finds
 * it); NULL when missing or not open. */
void *pw_vk_symbol(const char *name);
/* Free direct memory (the largest block: the kernel reports no total) and
 * free flexible memory, in MiB. */
void pw_vk_memory(unsigned long long *dmem_mib, unsigned long long *flex_mib);
/* libvulkan.prx's present counters; -1 when it is not loaded. */
int pw_vk_stats(struct pw_vulkan_stats *stats);

/* The frames vk_present draws: cyan, with a 64x64 orange square on
 * 240,135 copied in as bytes (R,G,B,A), so swapped red and blue fail both. */
#define PW_VK_E1_CLEAR_RGB 0, 255, 255
#define PW_VK_E1_MARK_RGB 255, 128, 0
#define PW_VK_E1_MARK_X 240
#define PW_VK_E1_MARK_Y 135
#define PW_VK_E1_FRAMES 330
#define PW_VK_E1_REOPEN_FRAMES 60

#endif
