/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The PS4 title's own display: two linear 1920x1080 scan-out buffers in
 * direct memory, filled by the CPU and flipped by VideoOut. The pixel
 * format is A8B8G8R8_SRGB, so memory holds R,G,B,A per pixel, the order
 * the b milestone's framebuffer readback measured and scripts/ps4/fbshot.sh
 * reads. (The PS5 title's tiled AGC path does not exist on the PS4.)
 */
#ifndef PW_VIDEOOUT_ORBIS_H
#define PW_VIDEOOUT_ORBIS_H

#include <stddef.h>
#include <stdint.h>

#define PW_VIDEOOUT_ORBIS_WIDTH 1920
#define PW_VIDEOOUT_ORBIS_HEIGHT 1080
#define PW_VIDEOOUT_ORBIS_BUFFERS 2

/* R,G,B,A in memory, as a little-endian word. */
#define PW_VIDEOOUT_ORBIS_RGBA(r, g, b) \
    ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | 0xff000000u)

typedef struct PwVideoOutOrbis {
    int32_t handle;
    uint32_t *buffers[PW_VIDEOOUT_ORBIS_BUFFERS];
    int64_t offset;
    size_t bytes;
    unsigned back;          /* the buffer the CPU fills next */
    unsigned shown;         /* the buffer flipped last */
    uint64_t flips;
    int status;             /* the first failing call's return, 0 once open */
    const char *failed;     /* that call's name */
} PwVideoOutOrbis;

/* 0, or the failing call's status (also in vo->status/failed). */
int pw_videoout_orbis_open(PwVideoOutOrbis *vo);
static inline uint32_t *pw_videoout_orbis_back(PwVideoOutOrbis *vo) { return vo->buffers[vo->back]; }
/* Flips the back buffer (vsync), then waits until no flip is pending. */
int pw_videoout_orbis_flip(PwVideoOutOrbis *vo);
/* Hands the display over (milestone e: RADV's WSI opens port 255 for the
 * swapchain and fails while it is open): waits up to 2 s for the last flip,
 * then closes the port. The buffers' direct memory stays allocated and
 * mapped: nx-on-orbis measured that freeing it after the close rebooted the
 * console once a game started. 0, or the close's status (also in
 * vo->status/failed); a vo this call closed already is 0. */
int pw_videoout_orbis_close(PwVideoOutOrbis *vo);
/* The buffer shown last, to <path> through <path>.tmp, fsync and rename:
 * the readback scripts/ps4/fbshot.sh fetches. 0 on success. */
int pw_videoout_orbis_save(const PwVideoOutOrbis *vo, const char *path);
/* The b milestone's pattern: black, a green square on 960,540, a 64x64
 * orange square on 240,135 (swapped red and blue fail the second). */
void pw_videoout_orbis_test_pattern(uint32_t *pixels);

#endif
