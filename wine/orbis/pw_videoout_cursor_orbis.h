/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * VideoOut's hardware cursor on the PS4: the pure parts of libvulkan.prx's
 * pw_videoout_cursor (pw_vulkan_radv_orbis.c) and of the title's probes of
 * it (gc1 on the title's own VideoOut, gc2 over RADV's frames), kept here so
 * the host tests (tests/test_pw_videoout_cursor_orbis.c) run them.
 *
 * VideoOut composes up to two 64x64 cursor images over whatever the output
 * shows (wine/ps5/pw_vulkan_radv.c measured it on the PS5). win32u hands the
 * application's cursor over as straight-alpha ARGB words (B8G8R8A8 bytes),
 * which this cuts to 64x64 and converts to what the PS4's plane reads.
 *
 * On the PS4, RADV's WSI (mesa-ps4 wsi_orbis.c) opens VideoOut when a
 * swapchain is created and closes it when the swapchain is destroyed, and
 * the port opens only once at a time, so the handle the cursor uses is
 * followed through both calls (pw_cursor_orbis_opened / _closed).
 */
#ifndef PW_VIDEOOUT_CURSOR_ORBIS_H
#define PW_VIDEOOUT_CURSOR_ORBIS_H

#include <stdint.h>
#include <string.h>

enum {
    PW_CURSOR_ORBIS_SIDE = 64,
    PW_CURSOR_ORBIS_PIXELS = PW_CURSOR_ORBIS_SIDE * PW_CURSOR_ORBIS_SIDE,
    PW_CURSOR_ORBIS_IMAGE_BYTES = PW_CURSOR_ORBIS_PIXELS * 4,   /* 16 KiB: one image */
};

/* What the conversion does to win32u's straight-alpha ARGB words. */
enum {
    PW_CURSOR_ORBIS_SWAP_RB = 1,       /* the plane reads R,G,B,A bytes */
    PW_CURSOR_ORBIS_PREMULTIPLY = 2,   /* the plane adds the colour over (1 - alpha) of the frame */
};

/* The probes' test images and their place: image A's 32x32 quadrants are
 * white, black, red and magenta at alpha 0 (top left, top right, bottom
 * left, bottom right) as ARGB words. With its top left corner at
 * (PW_CURSOR_ORBIS_TEST_X, _Y) of a 1920x1080 frame the quadrants' centres
 * are 944,524 (white), 976,524 (black), 944,556 (red) and 976,556
 * (transparent): the pixels scripts/ps4/check.sh reads for gc1 and gc2.
 * White and opaque black read the same whichever channel order the plane
 * uses; red tells the order, the transparent quadrant the alpha rule. */
#define PW_CURSOR_ORBIS_TEST_X 928
#define PW_CURSOR_ORBIS_TEST_Y 508
#define PW_CURSOR_ORBIS_TEST_MOVED_X 100
#define PW_CURSOR_ORBIS_TEST_MOVED_Y 100
#define PW_CURSOR_ORBIS_WHITE 0xffffffffu
#define PW_CURSOR_ORBIS_BLACK 0xff000000u
#define PW_CURSOR_ORBIS_RED 0xffff0000u
#define PW_CURSOR_ORBIS_CLEAR 0x00ff00ffu       /* magenta at alpha 0 */
#define PW_CURSOR_ORBIS_HALF_WHITE 0x80ffffffu  /* straight: half white */
#define PW_CURSOR_ORBIS_BLUE 0xff0000ffu

static inline void pw_cursor_orbis_test_image_a(uint32_t *pixels)
{
    for (unsigned y = 0; y < PW_CURSOR_ORBIS_SIDE; y++)
        for (unsigned x = 0; x < PW_CURSOR_ORBIS_SIDE; x++)
            pixels[y * PW_CURSOR_ORBIS_SIDE + x] = y < 32 ? (x < 32 ? PW_CURSOR_ORBIS_WHITE : PW_CURSOR_ORBIS_BLACK)
                                                          : (x < 32 ? PW_CURSOR_ORBIS_RED : PW_CURSOR_ORBIS_CLEAR);
}

/* Image B: the left half half-alpha white (straight: 127,255,127 over green;
 * premultiplied: white), the right half opaque blue (red when swapped). */
static inline void pw_cursor_orbis_test_image_b(uint32_t *pixels)
{
    for (unsigned y = 0; y < PW_CURSOR_ORBIS_SIDE; y++)
        for (unsigned x = 0; x < PW_CURSOR_ORBIS_SIDE; x++)
            pixels[y * PW_CURSOR_ORBIS_SIDE + x] = x < 32 ? PW_CURSOR_ORBIS_HALF_WHITE : PW_CURSOR_ORBIS_BLUE;
}

/* argb, width x height, into the 64x64 image out: its top left corner,
 * transparent where the cursor is smaller. */
static inline void pw_cursor_orbis_cut(uint32_t *out, const uint32_t *argb, uint32_t width, uint32_t height)
{
    uint32_t columns = width < PW_CURSOR_ORBIS_SIDE ? width : PW_CURSOR_ORBIS_SIDE;

    memset(out, 0, PW_CURSOR_ORBIS_IMAGE_BYTES);
    for (uint32_t row = 0; row < height && row < PW_CURSOR_ORBIS_SIDE; row++)
        memcpy(out + row * PW_CURSOR_ORBIS_SIDE, argb + (size_t)row * width, columns * sizeof(uint32_t));
}

static inline uint32_t pw_cursor_orbis_pixel(uint32_t argb, unsigned flags)
{
    uint32_t a = argb >> 24, r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;

    if (flags & PW_CURSOR_ORBIS_PREMULTIPLY) {
        r = (r * a + 127) / 255;
        g = (g * a + 127) / 255;
        b = (b * a + 127) / 255;
    }
    if (flags & PW_CURSOR_ORBIS_SWAP_RB) {
        uint32_t t = r;
        r = b;
        b = t;
    }
    return a << 24 | r << 16 | g << 8 | b;
}

static inline void pw_cursor_orbis_convert(uint32_t *pixels, unsigned flags)
{
    if (!flags) return;
    for (unsigned i = 0; i < PW_CURSOR_ORBIS_PIXELS; i++) pixels[i] = pw_cursor_orbis_pixel(pixels[i], flags);
}

/* The image's top left corner at (x, y) of a space_width x space_height
 * desktop, in the shown frame's pixels: RADV's WSI shows a swapchain 1:1 at
 * its own size (frame_width x frame_height), and win32u maps the desktop
 * onto it scaled from the top left. Left of or above the frame clamps to 0;
 * no frame size yet leaves the desktop's. */
static inline void pw_cursor_orbis_place(int32_t x, int32_t y, uint32_t space_width, uint32_t space_height,
                                         uint32_t frame_width, uint32_t frame_height, uint32_t *fx, uint32_t *fy)
{
    int64_t px = x > 0 ? x : 0, py = y > 0 ? y : 0;

    if (space_width && space_height && frame_width && frame_height) {
        px = px * frame_width / space_width;
        py = py * frame_height / space_height;
    }
    *fx = (uint32_t)px;
    *fy = (uint32_t)py;
}

/* The handle the cursor uses: the last one opened, until that one closes.
 * A close of another (an older swapchain's, closed after the new one
 * opened) leaves it; 1 when the cursor's own output closed. */
static inline void pw_cursor_orbis_opened(int *current, int handle)
{
    if (handle >= 0) *current = handle;
}

static inline int pw_cursor_orbis_closed(int *current, int handle)
{
    if (handle < 0 || *current != handle) return 0;
    *current = -1;
    return 1;
}

/* A sceVideoOut status for a line: "0", or its hex. */
static inline const char *pw_cursor_orbis_rc(int rc, char *out, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t v = (uint32_t)rc;
    size_t n = 0;
    char digits[8];
    int count = 0;

    if (!size) return out;
    if (!rc) {
        if (size > 1) out[n++] = '0';
        out[n] = 0;
        return out;
    }
    do digits[count++] = hex[v & 15]; while ((v >>= 4) && count < 8);
    if (size > 2) {
        out[n++] = '0';
        out[n++] = 'x';
    }
    while (count && n + 1 < size) out[n++] = digits[--count];
    out[n] = 0;
    return out;
}

#endif
