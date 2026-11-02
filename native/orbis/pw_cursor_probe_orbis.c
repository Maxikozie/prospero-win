/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The title's probes of VideoOut's hardware cursor (pw_cursor_probe_orbis.h).
 * The calls' signatures are the ones wine/ps5/pw_vulkan_radv.c uses on the
 * PS5 (the SDK headers declare them without arguments). */
#include "pw_cursor_probe_orbis.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pw_videoout_cursor_orbis.h"
#include "pw_videoout_orbis.h"
#include "pw_vk_stage_orbis.h"
#include "pw_vulkan_radv_orbis.h"

extern int sceVideoOutCursorEnable(int32_t handle, int32_t index, const void *address);
extern int sceVideoOutCursorDisable(int32_t handle, int32_t index);
extern int sceVideoOutCursorSetImageAddress(int32_t handle, int32_t index, const void *address);
extern int sceVideoOutCursorSetPosition(int32_t handle, int32_t index, uint32_t x, uint32_t y);
extern size_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t bytes, size_t alignment, int type,
                                         int64_t *offset);
extern int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags, int64_t offset,
                                    size_t alignment);

enum { MEMORY = 64 << 10, GARLIC = 3, CPU_GPU_READ_WRITE = 0x33, ALIGN_2MIB = 2 << 20 };

static PwCursorProbeHost host;

static void pstep(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void pstep(const char *format, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    if (host.step) host.step(line);
}

/* ---- gc1: the four calls on the title's own VideoOut --------------------- */

#define GREEN PW_VIDEOOUT_ORBIS_RGBA(0, 255, 0)
#define WHITE PW_VIDEOOUT_ORBIS_RGBA(255, 255, 255)
enum { BARS_X = 32, BARS_Y = 996, BAR = 40, BAR_STEP = 56, BARS_W = PW_CURSOR_PROBE_STATES * BAR_STEP + 16, BARS_H = 56 };

/* The state's number as white bars on green at the bottom left. */
static void draw_bars(const PwVideoOutOrbis *vo, uint32_t *pixels, unsigned state)
{
    uint32_t pitch = vo->mode.pitch;
    for (unsigned y = BARS_Y; y < BARS_Y + BARS_H; y++)
        for (unsigned x = BARS_X; x < BARS_X + BARS_W; x++) {
            unsigned i = (x - BARS_X - 8) / BAR_STEP, in_x = (x - BARS_X - 8) % BAR_STEP;
            int bar = x >= BARS_X + 8 && i < state && in_x < BAR && y >= BARS_Y + 8 && y < BARS_Y + 8 + BAR;
            pixels[(size_t)y * pitch + x] = bar ? WHITE : GREEN;
        }
}

static void fill(const PwVideoOutOrbis *vo, unsigned state)
{
    for (unsigned b = 0; b < vo->mode.buffers; b++) {
        uint32_t *pixels = vo->buffers[b];
        for (size_t i = 0; i < (size_t)vo->mode.pitch * vo->mode.height; i++) pixels[i] = GREEN;
        draw_bars(vo, pixels, state);
    }
}

/* Flips every vblank for ms, the state's bars in each back buffer. */
static void show(PwVideoOutOrbis *vo, unsigned state, unsigned ms, unsigned *flips, unsigned *errors)
{
    uint64_t end = host.now_ns() + (uint64_t)ms * 1000000u;
    while (host.now_ns() < end) {
        draw_bars(vo, pw_videoout_orbis_back(vo), state);
        if (pw_videoout_orbis_flip(vo)) {
            (*errors)++;
            usleep(16000);
        } else {
            (*flips)++;
        }
    }
}

static void flush(const void *address, size_t bytes)
{
    for (size_t at = 0; at < bytes; at += 64) __builtin_ia32_clflush((const uint8_t *)address + at);
    __builtin_ia32_mfence();
}

/* The images' memory as libvulkan.prx takes it: 64 KiB of Garlic, 64 KiB
 * aligned; 2 MiB aligned when that is refused. */
static uint32_t *cursor_memory(size_t *alignment, int *first_rc)
{
    static const size_t alignments[] = { MEMORY, ALIGN_2MIB };
    *first_rc = 0;
    for (unsigned i = 0; i < 2; i++) {
        int64_t offset;
        void *address = NULL;
        int rc = sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), MEMORY, alignments[i],
                                               GARLIC, &offset);
        if (rc >= 0) rc = sceKernelMapDirectMemory(&address, MEMORY, CPU_GPU_READ_WRITE, 0, offset, alignments[i]);
        pstep("gc1 cursor memory align=%zu rc=0x%x address=%p", alignments[i], (unsigned)rc, address);
        if (rc >= 0 && address) {
            *alignment = alignments[i];
            return address;
        }
        if (!*first_rc) *first_rc = rc ? rc : -1;
    }
    *alignment = 0;
    return NULL;
}

void pw_cursor_probe_videoout(const PwCursorProbeHost *probe_host, char *fields, size_t size)
{
    PwVideoOutOrbis vo;
    uint32_t *images, *a, *b;
    size_t alignment = 0;
    unsigned flips = 0, errors = 0;
    int mem_rc, rc, enable = 1, position = 1, enable_twice = 1, moved = 1, back = 1, image = 1, edge = 1, disable = 1,
        again = 1, reopen = 0, reopen_disable = 1;
    const char *reopen_text = "-";
    char t[13][16];

    host = *probe_host;
    if ((rc = pw_videoout_orbis_open(&vo))) {
        snprintf(fields, size, "cursor=fail mem=- vo_failed=%s:0x%x", vo.failed ? vo.failed : "-", (unsigned)rc);
        return;
    }
    fill(&vo, 1);
    pstep("gc1 videoout handle=%d; state 1: green only", vo.handle);
    show(&vo, 1, PW_CURSOR_PROBE_STATE_MS, &flips, &errors);
    if (!(images = cursor_memory(&alignment, &mem_rc))) {
        pw_videoout_orbis_close(&vo);
        snprintf(fields, size, "cursor=fail mem=fail mem_rc=%s flips=%u flip_errors=%u",
                 pw_cursor_orbis_rc(mem_rc, t[0], sizeof(t[0])), flips, errors);
        return;
    }
    a = images;
    b = images + PW_CURSOR_ORBIS_PIXELS;
    pw_cursor_orbis_test_image_a(a);
    pw_cursor_orbis_test_image_b(b);
    flush(images, 2 * PW_CURSOR_ORBIS_IMAGE_BYTES);

    /* 2: image A at the test place (the gated frame). */
    enable = sceVideoOutCursorEnable(vo.handle, 0, a);
    position = sceVideoOutCursorSetPosition(vo.handle, 0, PW_CURSOR_ORBIS_TEST_X, PW_CURSOR_ORBIS_TEST_Y);
    pstep("gc1 state 2: enable=0x%x position(%d,%d)=0x%x", (unsigned)enable, PW_CURSOR_ORBIS_TEST_X,
          PW_CURSOR_ORBIS_TEST_Y, (unsigned)position);
    show(&vo, 2, PW_CURSOR_PROBE_STATE_MS, &flips, &errors);
    /* 3: enabled again while enabled, then moved. */
    enable_twice = sceVideoOutCursorEnable(vo.handle, 0, a);
    moved = sceVideoOutCursorSetPosition(vo.handle, 0, PW_CURSOR_ORBIS_TEST_MOVED_X, PW_CURSOR_ORBIS_TEST_MOVED_Y);
    pstep("gc1 state 3: enable_twice=0x%x position(%d,%d)=0x%x", (unsigned)enable_twice, PW_CURSOR_ORBIS_TEST_MOVED_X,
          PW_CURSOR_ORBIS_TEST_MOVED_Y, (unsigned)moved);
    show(&vo, 3, PW_CURSOR_PROBE_STATE_MS, &flips, &errors);
    /* 4: back at the test place, image B. */
    back = sceVideoOutCursorSetPosition(vo.handle, 0, PW_CURSOR_ORBIS_TEST_X, PW_CURSOR_ORBIS_TEST_Y);
    image = sceVideoOutCursorSetImageAddress(vo.handle, 0, b);
    pstep("gc1 state 4: position=0x%x image=0x%x", (unsigned)back, (unsigned)image);
    show(&vo, 4, PW_CURSOR_PROBE_STATE_MS, &flips, &errors);
    /* 5: partly off the frame. */
    edge = sceVideoOutCursorSetPosition(vo.handle, 0, 1900, 1060);
    pstep("gc1 state 5: position(1900,1060)=0x%x", (unsigned)edge);
    show(&vo, 5, PW_CURSOR_PROBE_STATE_MS, &flips, &errors);
    /* 6: hidden. */
    disable = sceVideoOutCursorDisable(vo.handle, 0);
    pstep("gc1 state 6: disable=0x%x", (unsigned)disable);
    show(&vo, 6, PW_CURSOR_PROBE_STATE_MS, &flips, &errors);
    /* 7: shown again after the disable, image A at the test place. */
    again = sceVideoOutCursorEnable(vo.handle, 0, a);
    rc = sceVideoOutCursorSetPosition(vo.handle, 0, PW_CURSOR_ORBIS_TEST_X, PW_CURSOR_ORBIS_TEST_Y);
    pstep("gc1 state 7: enable_again=0x%x position=0x%x", (unsigned)again, (unsigned)rc);
    show(&vo, 7, PW_CURSOR_PROBE_STATE_MS, &flips, &errors);
    /* 8: the port closed with the cursor shown, opened again, no cursor call:
     * does the cursor outlive its handle? */
    rc = pw_videoout_orbis_close(&vo);
    pstep("gc1 state 8: close=0x%x", (unsigned)rc);
    if (!rc) reopen_text = (reopen = pw_videoout_orbis_open(&vo)) ? pw_cursor_orbis_rc(reopen, t[10], sizeof(t[10])) : "ok";
    if (!rc && !reopen) {
        fill(&vo, 8);
        pstep("gc1 state 8: reopened handle=%d", vo.handle);
        show(&vo, 8, PW_CURSOR_PROBE_STATE_MS, &flips, &errors);
        reopen_disable = sceVideoOutCursorDisable(vo.handle, 0);
        pstep("gc1 state 8: disable on the new handle=0x%x", (unsigned)reopen_disable);
        pw_videoout_orbis_close(&vo);
    }
    snprintf(fields, size,
             "cursor=%s mem=ok mem_align=%zu mem_rc=%s enable=%s position=%s image=%s disable=%s enable_twice=%s "
             "position_moved=%s position_back=%s position_edge=%s enable_again=%s reopen=%s reopen_disable=%s "
             "flips=%u flip_errors=%u",
             !enable && !position && !image && !disable ? "ok" : "fail", alignment,
             pw_cursor_orbis_rc(mem_rc, t[0], sizeof(t[0])), pw_cursor_orbis_rc(enable, t[1], sizeof(t[1])),
             pw_cursor_orbis_rc(position, t[2], sizeof(t[2])), pw_cursor_orbis_rc(image, t[3], sizeof(t[3])),
             pw_cursor_orbis_rc(disable, t[4], sizeof(t[4])), pw_cursor_orbis_rc(enable_twice, t[5], sizeof(t[5])),
             pw_cursor_orbis_rc(moved, t[6], sizeof(t[6])), pw_cursor_orbis_rc(back, t[7], sizeof(t[7])),
             pw_cursor_orbis_rc(edge, t[8], sizeof(t[8])), pw_cursor_orbis_rc(again, t[9], sizeof(t[9])),
             reopen_text, strcmp(reopen_text, "ok") ? "-" : pw_cursor_orbis_rc(reopen_disable, t[11], sizeof(t[11])),
             flips, errors);
}

/* ---- gc2: libvulkan.prx's pw_videoout_cursor over RADV's frames ----------- */

typedef int (*CursorFn)(const uint32_t *argb, uint32_t width, uint32_t height, int32_t x, int32_t y,
                        uint32_t space_width, uint32_t space_height);

typedef struct Gc2 {
    CursorFn cursor;
    uint32_t image[PW_CURSOR_ORBIS_PIXELS];
    unsigned long long shows, hides, noout, errors, reopen_shows;
    int last_rc, phase;
} Gc2;

static void gc2_call(Gc2 *g, int phase, const uint32_t *image, int32_t x, int32_t y)
{
    int rc = g->cursor(image, PW_CURSOR_ORBIS_SIDE, PW_CURSOR_ORBIS_SIDE, x, y, 1920, 1080);

    if (rc == 0 && image && phase == 5) g->reopen_shows++;
    if (rc == 0 && image) g->shows++;
    else if (rc == 0) g->hides++;
    else if (rc == 1) g->noout++;
    else {
        g->errors++;
        g->last_rc = rc;
    }
    if (phase != g->phase) {
        g->phase = phase;
        pstep("gc2 phase %d: rc=%d (shows=%llu hides=%llu noout=%llu errors=%llu)", phase, rc, g->shows, g->hides,
              g->noout, g->errors);
    }
}

/* After each present, as win32u calls it: image A at the test place, moved,
 * hidden; in the second swapchain at the test place again. */
static void gc2_frame(void *context, unsigned swapchain, unsigned index)
{
    Gc2 *g = context;

    if (swapchain) gc2_call(g, 5, g->image, PW_CURSOR_ORBIS_TEST_X, PW_CURSOR_ORBIS_TEST_Y);
    else if (index < 300) gc2_call(g, 1, g->image, PW_CURSOR_ORBIS_TEST_X, PW_CURSOR_ORBIS_TEST_Y);
    else if (index < 600) gc2_call(g, 2, g->image, PW_CURSOR_ORBIS_TEST_MOVED_X, PW_CURSOR_ORBIS_TEST_MOVED_Y);
    else gc2_call(g, 3, NULL, 0, 0);
}

/* No swapchain: VideoOut is closed, so the cursor has no output (rc 1). */
static void gc2_between(void *context)
{
    Gc2 *g = context;
    gc2_call(g, 4, g->image, PW_CURSOR_ORBIS_TEST_X, PW_CURSOR_ORBIS_TEST_Y);
}

/* RADV's readback (R,G,B,A rows of 1920) at a pixel: the scan-out's own
 * frame, which VideoOut's cursor plane is not part of. */
static void shot_pixel(const char *shot, unsigned x, unsigned y, char *out, size_t size)
{
    uint8_t px[4];
    FILE *file = fopen(shot, "rb");

    snprintf(out, size, "-");
    if (!file) return;
    if (!fseek(file, ((long)y * 1920 + x) * 4, SEEK_SET) && fread(px, 1, 4, file) == 4)
        snprintf(out, size, "%02x%02x%02x", px[0], px[1], px[2]);
    fclose(file);
}

void pw_cursor_probe_vulkan(const PwCursorProbeHost *probe_host, const char *shot, char *fields, size_t size)
{
    static Gc2 g;
    struct pw_vulkan_cursor_stats prx;
    int (*prx_stats)(struct pw_vulkan_cursor_stats *);
    PwVkFrames frames = { PW_CURSOR_PROBE_VK_FIRST, PW_CURSOR_PROBE_VK_REOPEN, gc2_frame, gc2_between, &g };
    char present[700], rc[16], prx_rc[16], fb[8];
    const char *prx_state = "nosym";

    host = *probe_host;
    memset(&g, 0, sizeof(g));
    g.phase = -1;
    if (!(g.cursor = (CursorFn)(uintptr_t)pw_vk_symbol("pw_videoout_cursor"))) {
        snprintf(fields, size, "present=- cursor_export=none");
        return;
    }
    pw_cursor_orbis_test_image_a(g.image);
    pw_vk_present_frames(shot, &frames, present, sizeof(present));
    memset(&prx, 0, sizeof(prx));
    prx.size = sizeof(prx);
    prx_stats = (int (*)(struct pw_vulkan_cursor_stats *))(uintptr_t)pw_vk_symbol("pw_vulkan_cursor_stats");
    if (prx_stats) prx_state = prx_stats(&prx) ? "readfailed" : "ok";
    shot_pixel(shot, PW_CURSOR_ORBIS_TEST_X + 16, PW_CURSOR_ORBIS_TEST_Y + 16, fb, sizeof(fb));
    snprintf(fields, size,
             "%s cursor_export=ok cursor_shows=%llu cursor_reopen_shows=%llu cursor_hides=%llu cursor_noout=%llu "
             "cursor_errors=%llu cursor_rc=%s vk_cursor=%s vk_cursor_updates=%llu vk_cursor_errors=%llu vk_cursor_rc=%s "
             "vk_cursor_format=%u fb_cursor=%s",
             present, g.shows, g.reopen_shows, g.hides, g.noout, g.errors, pw_cursor_orbis_rc(g.last_rc, rc, sizeof(rc)), prx_state,
             (unsigned long long)prx.updates, (unsigned long long)prx.errors,
             pw_cursor_orbis_rc(prx.last_rc, prx_rc, sizeof(prx_rc)), prx.format, fb);
}
