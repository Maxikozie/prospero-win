/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The PS4 title's probes of VideoOut's hardware cursor (milestone g's
 * input step):
 *
 *   gc1  the four calls (enable, image address, position, disable) on the
 *        title's own VideoOut, no Wine and no RADV: a green 1920x1080 frame
 *        flipped every vblank, test image A (pw_videoout_cursor_orbis.h)
 *        shown, moved, swapped for image B, put at the edge, hidden, shown
 *        again, then the port closed and opened again. Each state lasts
 *        PW_CURSOR_PROBE_STATE_MS and is numbered on screen by white bars at
 *        the bottom left, so a frame of the HDMI recording names its state.
 *   gc2  libvulkan.prx's pw_videoout_cursor, found as win32u finds it and
 *        called after every present of e1's frames (pw_vk_present_frames),
 *        through a swapchain destroyed and made again.
 *
 * Each fills one stage's "key=value ..." fields.
 */
#ifndef PW_CURSOR_PROBE_ORBIS_H
#define PW_CURSOR_PROBE_ORBIS_H

#include <stddef.h>
#include <stdint.h>

#define PW_CURSOR_PROBE_STATE_MS 6000
#define PW_CURSOR_PROBE_STATES 8
/* gc2: frames of the first swapchain (image A at the test place to 300,
 * moved to 600, hidden after) and of the second (image A at the test place). */
#define PW_CURSOR_PROBE_VK_FIRST 900
#define PW_CURSOR_PROBE_VK_REOPEN 360

typedef struct PwCursorProbeHost {
    void (*step)(const char *line);   /* one progress record */
    uint64_t (*now_ns)(void);
} PwCursorProbeHost;

void pw_cursor_probe_videoout(const PwCursorProbeHost *host, char *fields, size_t size);
/* gc2, after pw_vk_open; shot names RADV's ORBIS_WSI_SHOT file. */
void pw_cursor_probe_vulkan(const PwCursorProbeHost *host, const char *shot, char *fields, size_t size);

#endif
