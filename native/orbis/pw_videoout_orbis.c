/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The PS4 title's linear VideoOut display (pw_videoout_orbis.h). The call
 * sequence and constants are the ones the PS4 port's milestone a probe and
 * mesa-ps4's wsi_orbis.c use on the console: Garlic (write-combined) direct
 * memory mapped CPU+GPU read-write, 2 MiB aligned, VideoOut port 255 of the
 * system user, a linear A8B8G8R8_SRGB attribute, flip rate 0 (60 Hz). */
#include "pw_videoout_orbis.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t bytes, size_t alignment, int type,
                                         int64_t *offset);
extern int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags,
                                    int64_t offset, size_t alignment);
extern size_t sceKernelGetDirectMemorySize(void);
extern int32_t sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *param);
extern void sceVideoOutSetBufferAttribute(void *attribute, uint32_t format, uint32_t tiling, uint32_t aspect,
                                          uint32_t width, uint32_t height, uint32_t pitch);
extern int32_t sceVideoOutRegisterBuffers(int32_t handle, int32_t start, void *const *addresses, int32_t count,
                                          const void *attribute);
extern int32_t sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
extern int32_t sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t argument);
extern int32_t sceVideoOutIsFlipPending(int32_t handle);
extern int32_t sceVideoOutClose(int32_t handle);
/* OrbisVideoOutFlipStatus (OpenOrbis _types/video.h). */
typedef struct FlipStatus {
    uint64_t count, process_time, tsc;
    int64_t flip_arg;
    uint64_t reserved[2];
    int32_t gpu_flips_pending, flips_pending, current_buffer;
    uint32_t reserved1;
} FlipStatus;
extern int32_t sceVideoOutGetFlipStatus(int32_t handle, FlipStatus *status);
extern int32_t sceKernelUsleep(uint32_t microseconds);

enum {
    WC_GARLIC = 3,
    CPU_GPU_READ_WRITE = 0x33,
    ALIGN_2MIB = 2 << 20,
    VIDEOOUT_USER_SYSTEM = 0xff,
    VIDEOOUT_BUS_MAIN = 0,
    FORMAT_A8B8G8R8_SRGB = 0x80002200,
    FORMAT_A8R8G8B8_SRGB = 0x80000000,   /* the milestone a probe's display format (probes/probe.c) */
    TILING_LINEAR = 1,
    FLIP_VSYNC = 1,
};

static int fail(PwVideoOutOrbis *vo, const char *call, int status)
{
    vo->status = status ? status : -1;
    vo->failed = call;
    return vo->status;
}

static const PwVideoOutOrbisMode default_mode = {
    PW_VIDEOOUT_ORBIS_WIDTH, PW_VIDEOOUT_ORBIS_HEIGHT, PW_VIDEOOUT_ORBIS_WIDTH, 0, PW_VIDEOOUT_ORBIS_BUFFERS, 0,
};

int pw_videoout_orbis_open(PwVideoOutOrbis *vo)
{
    return pw_videoout_orbis_open_mode(vo, NULL);
}

int pw_videoout_orbis_open_mode(PwVideoOutOrbis *vo, const PwVideoOutOrbisMode *mode)
{
    size_t frame, each;
    uint8_t attribute[64];
    void *base = NULL, *addresses[PW_VIDEOOUT_ORBIS_MAX_BUFFERS];
    int rc;

    memset(vo, 0, sizeof(*vo));
    vo->handle = -1;
    vo->mode = mode ? *mode : default_mode;
    if (vo->mode.buffers < 2 || vo->mode.buffers > PW_VIDEOOUT_ORBIS_MAX_BUFFERS || !vo->mode.width ||
        !vo->mode.height || vo->mode.pitch < vo->mode.width)
        return fail(vo, "mode", -1);
    frame = (size_t)vo->mode.pitch * vo->mode.height * 4;
    each = (frame + ALIGN_2MIB - 1) & ~(size_t)(ALIGN_2MIB - 1);
    vo->bytes = each * vo->mode.buffers;
    if ((rc = sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), vo->bytes, ALIGN_2MIB,
                                            WC_GARLIC, &vo->offset)))
        return fail(vo, "sceKernelAllocateDirectMemory", rc);
    if ((rc = sceKernelMapDirectMemory(&base, vo->bytes, CPU_GPU_READ_WRITE, 0, vo->offset, ALIGN_2MIB)))
        return fail(vo, "sceKernelMapDirectMemory", rc);
    for (unsigned i = 0; i < vo->mode.buffers; i++) {
        vo->buffers[i] = (uint32_t *)((uint8_t *)base + i * each);
        addresses[i] = vo->buffers[i];
        memset(vo->buffers[i], 0, frame);
    }
    if ((vo->handle = sceVideoOutOpen(VIDEOOUT_USER_SYSTEM, VIDEOOUT_BUS_MAIN, 0, NULL)) < 0)
        return fail(vo, "sceVideoOutOpen", vo->handle);
    memset(attribute, 0, sizeof(attribute));
    sceVideoOutSetBufferAttribute(attribute, vo->mode.bgra ? FORMAT_A8R8G8B8_SRGB : FORMAT_A8B8G8R8_SRGB,
                                  TILING_LINEAR, 0, vo->mode.width, vo->mode.height, vo->mode.pitch);
    if ((rc = sceVideoOutSetFlipRate(vo->handle, 0))) return fail(vo, "sceVideoOutSetFlipRate", rc);
    if ((rc = sceVideoOutRegisterBuffers(vo->handle, 0, addresses, (int32_t)vo->mode.buffers, attribute)) < 0)
        return fail(vo, "sceVideoOutRegisterBuffers", rc);
    vo->back = 0;
    vo->shown = vo->mode.buffers - 1;
    return 0;
}

int pw_videoout_orbis_flip(PwVideoOutOrbis *vo)
{
    struct timespec pause = { 0, 1000000 };
    /* The argument is the flip's number from 1, which pw_videoout_orbis_shown reads back. */
    int rc = sceVideoOutSubmitFlip(vo->handle, (int32_t)vo->back, FLIP_VSYNC, (int64_t)vo->flips + 1);
    if (rc) {
        vo->flip_errors++;
        return fail(vo, "sceVideoOutSubmitFlip", rc);
    }
    if (!vo->mode.async)
        for (int i = 0; i < 100 && sceVideoOutIsFlipPending(vo->handle) > 0; i++) nanosleep(&pause, NULL);
    vo->shown = vo->back;
    vo->back = (vo->back + 1) % vo->mode.buffers;
    vo->flips++;
    return 0;
}

int pw_videoout_orbis_wait_slot(PwVideoOutOrbis *vo)
{
    const int32_t limit = (int32_t)vo->mode.buffers - 1;
    int waited = 0;

    if (!vo->mode.async || vo->handle < 0) return 0;
    for (unsigned i = 0;; i++) {
        FlipStatus st;
        memset(&st, 0, sizeof(st));
        if (sceVideoOutGetFlipStatus(vo->handle, &st)) {
            /* Cannot see the queue: go ahead rather than stall, and count it. */
            vo->status_errors++;
            return waited;
        }
        if (st.flips_pending < limit) return waited;
        if (!waited) vo->slot_waits++;
        waited = 1;
        if (i >= 4000) {   /* 4000 x 500 us: the display stopped taking flips */
            vo->slot_timeouts++;
            vo->flip_errors++;
            return waited;
        }
        sceKernelUsleep(500);
    }
}

int pw_videoout_orbis_shown(PwVideoOutOrbis *vo, int64_t *flip_arg)
{
    FlipStatus st;
    int rc;

    memset(&st, 0, sizeof(st));
    *flip_arg = 0;
    if (vo->handle < 0) return -1;
    if ((rc = sceVideoOutGetFlipStatus(vo->handle, &st))) return rc;
    *flip_arg = st.flip_arg;
    return 0;
}

int pw_videoout_orbis_close(PwVideoOutOrbis *vo)
{
    struct timespec pause = { 0, 1000000 };
    int rc;

    if (vo->handle < 0) return 0;
    for (int i = 0; i < 2000 && sceVideoOutIsFlipPending(vo->handle) > 0; i++) nanosleep(&pause, NULL);
    rc = sceVideoOutClose(vo->handle);
    vo->handle = -1;
    return rc ? fail(vo, "sceVideoOutClose", rc) : 0;
}

int pw_videoout_orbis_save(const PwVideoOutOrbis *vo, const char *path)
{
    const size_t frame = (size_t)(vo->mode.pitch ? vo->mode.pitch : PW_VIDEOOUT_ORBIS_WIDTH) *
                         (vo->mode.height ? vo->mode.height : PW_VIDEOOUT_ORBIS_HEIGHT) * 4;
    const uint8_t *pixels = (const uint8_t *)vo->buffers[vo->shown];
    char tmp[512];
    FILE *file;
    int ok;

    /* stdio, as the milestone a probe's result files (measured to survive a
     * crash on /data), rather than open() with flag values to translate. */
    if (!pixels) return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (!(file = fopen(tmp, "wb"))) return -1;
    ok = fwrite(pixels, 1, frame, file) == frame && fflush(file) == 0 && fsync(fileno(file)) == 0;
    ok = fclose(file) == 0 && ok;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

static void fill_square(uint32_t *pixels, int cx, int cy, int half, uint32_t colour)
{
    for (int y = cy - half; y < cy + half; y++)
        for (int x = cx - half; x < cx + half; x++)
            pixels[(size_t)y * PW_VIDEOOUT_ORBIS_WIDTH + (size_t)x] = colour;
}

void pw_videoout_orbis_test_pattern(uint32_t *pixels)
{
    for (size_t i = 0; i < (size_t)PW_VIDEOOUT_ORBIS_WIDTH * PW_VIDEOOUT_ORBIS_HEIGHT; i++)
        pixels[i] = PW_VIDEOOUT_ORBIS_RGBA(0, 0, 0);
    fill_square(pixels, 960, 540, 200, PW_VIDEOOUT_ORBIS_RGBA(0, 255, 0));
    fill_square(pixels, 240, 135, 32, PW_VIDEOOUT_ORBIS_RGBA(255, 128, 0));
}
