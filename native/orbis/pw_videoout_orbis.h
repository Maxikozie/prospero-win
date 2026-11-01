/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The PS4 title's own display: two linear 1920x1080 scan-out buffers in
 * direct memory, filled by the CPU and flipped by VideoOut. The pixel
 * format is A8B8G8R8_SRGB, so memory holds R,G,B,A per pixel, the order
 * the b milestone's framebuffer readback measured and scripts/ps4/fbshot.sh
 * reads. (The PS5 title's tiled AGC path does not exist on the PS4.)
 *
 * Phase p (docs/ps4/HANDOFF-p.md in ps4-wine) opens it in other modes too:
 * a smaller size with its own pitch, A8R8G8B8_SRGB (memory B,G,R,A, the
 * user driver's order, so a frame is a row copy), three buffers, and an
 * asynchronous flip that returns once submitted and waits only for a free
 * buffer: proceed while fewer than count-1 flips are pending, as mesa-ps4's
 * wsi_orbis.c does (it measured the display's queue overflowing, 21 flips in
 * ~110 ms, without that limit). Whether VideoOut scales a buffer smaller than
 * the output to it is measured by p3, not assumed here.
 */
#ifndef PW_VIDEOOUT_ORBIS_H
#define PW_VIDEOOUT_ORBIS_H

#include <stddef.h>
#include <stdint.h>

#define PW_VIDEOOUT_ORBIS_WIDTH 1920
#define PW_VIDEOOUT_ORBIS_HEIGHT 1080
#define PW_VIDEOOUT_ORBIS_BUFFERS 2          /* the default mode's */
#define PW_VIDEOOUT_ORBIS_MAX_BUFFERS 3

/* R,G,B,A in memory, as a little-endian word. */
#define PW_VIDEOOUT_ORBIS_RGBA(r, g, b) \
    ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | 0xff000000u)

typedef struct PwVideoOutOrbisMode {
    uint32_t width, height, pitch;   /* pixels; pitch >= width */
    int bgra;                        /* 1: A8R8G8B8_SRGB (memory B,G,R,A); 0: A8B8G8R8_SRGB (R,G,B,A) */
    unsigned buffers;                /* 2 or 3 */
    int async;                       /* 1: the flip returns once submitted (pw_videoout_orbis_wait_slot) */
} PwVideoOutOrbisMode;

typedef struct PwVideoOutOrbis {
    int32_t handle;
    PwVideoOutOrbisMode mode;
    uint32_t *buffers[PW_VIDEOOUT_ORBIS_MAX_BUFFERS];
    int64_t offset;
    size_t bytes;
    unsigned back;          /* the buffer the CPU fills next */
    unsigned shown;         /* the buffer flipped last */
    uint64_t flips;
    int status;             /* the first failing call's return, 0 once open */
    const char *failed;     /* that call's name */
    /* Counted from open: submits refused, waits for a free buffer (async),
     * waits that ran out (2 s), flip-status reads that failed. */
    uint64_t flip_errors, slot_waits, slot_timeouts, status_errors;
} PwVideoOutOrbis;

/* 0, or the failing call's status (also in vo->status/failed): the default
 * mode, 1920x1080 A8B8G8R8_SRGB, two buffers, a synchronous flip. */
int pw_videoout_orbis_open(PwVideoOutOrbis *vo);
/* The same in another mode (NULL: the default). */
int pw_videoout_orbis_open_mode(PwVideoOutOrbis *vo, const PwVideoOutOrbisMode *mode);
/* Async mode: waits (sceKernelUsleep, at most 2 s) until fewer than
 * count-1 flips are pending, so the next back buffer is neither queued nor
 * on screen; 1 when it had to wait, 0 when it did not (or in sync mode). */
int pw_videoout_orbis_wait_slot(PwVideoOutOrbis *vo);
/* The argument of the last flip the display completed (pw_videoout_orbis_flip
 * passes 1, 2, 3, ...), so the flips shown between two reads are their
 * difference; 0, or sceVideoOutGetFlipStatus's error. */
int pw_videoout_orbis_shown(PwVideoOutOrbis *vo, int64_t *flip_arg);
static inline uint32_t *pw_videoout_orbis_back(PwVideoOutOrbis *vo) { return vo->buffers[vo->back]; }
/* Flips the back buffer (vsync); in sync mode then waits until no flip is
 * pending, in async mode returns. */
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
