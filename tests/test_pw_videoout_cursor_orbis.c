/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../wine/orbis/pw_videoout_cursor_orbis.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t image[PW_CURSOR_ORBIS_PIXELS], source[128 * 128];

int main(void)
{
    uint32_t fx, fy;
    int current = -1;
    char rc[16];

    /* The test image: the quadrants check.sh reads, at their centres relative to the corner. */
    pw_cursor_orbis_test_image_a(image);
    assert(image[16 * 64 + 16] == PW_CURSOR_ORBIS_WHITE && image[16 * 64 + 48] == PW_CURSOR_ORBIS_BLACK);
    assert(image[48 * 64 + 16] == PW_CURSOR_ORBIS_RED && image[48 * 64 + 48] == PW_CURSOR_ORBIS_CLEAR);
    assert(PW_CURSOR_ORBIS_TEST_X + 16 == 944 && PW_CURSOR_ORBIS_TEST_Y + 16 == 524);
    assert(PW_CURSOR_ORBIS_TEST_X + 48 == 976 && PW_CURSOR_ORBIS_TEST_Y + 48 == 556);
    pw_cursor_orbis_test_image_b(image);
    assert(image[0] == PW_CURSOR_ORBIS_HALF_WHITE && image[63] == PW_CURSOR_ORBIS_BLUE);

    /* A 32x32 cursor: its rows at the top left, the rest transparent. */
    for (unsigned i = 0; i < 32 * 32; i++) source[i] = 0xff000000u | i;
    memset(image, 0xaa, sizeof(image));
    pw_cursor_orbis_cut(image, source, 32, 32);
    assert(image[0] == 0xff000000u && image[31] == (0xff000000u | 31) && image[32] == 0);
    assert(image[64] == (0xff000000u | 32) && image[31 * 64 + 31] == (0xff000000u | (32 * 32 - 1)));
    assert(image[32 * 64] == 0 && image[63 * 64 + 63] == 0);
    /* A 128x128 cursor: its top left 64x64. */
    for (unsigned i = 0; i < 128 * 128; i++) source[i] = i;
    pw_cursor_orbis_cut(image, source, 128, 128);
    assert(image[63] == 63 && image[64] == 128 && image[63 * 64 + 63] == 63 * 128 + 63);
    /* A 1-pixel-wide one. */
    pw_cursor_orbis_cut(image, source, 1, 3);
    assert(image[0] == 0 && image[64] == 1 && image[128] == 2 && image[1] == 0 && image[192] == 0);

    /* The conversion: none, the red/blue swap, premultiplication, both. */
    assert(pw_cursor_orbis_pixel(0xff112233u, 0) == 0xff112233u);
    assert(pw_cursor_orbis_pixel(0xff112233u, PW_CURSOR_ORBIS_SWAP_RB) == 0xff332211u);
    assert(pw_cursor_orbis_pixel(0x80ffffffu, PW_CURSOR_ORBIS_PREMULTIPLY) == 0x80808080u);
    assert(pw_cursor_orbis_pixel(0x00ff00ffu, PW_CURSOR_ORBIS_PREMULTIPLY) == 0x00000000u);
    assert(pw_cursor_orbis_pixel(0xffff0000u, PW_CURSOR_ORBIS_PREMULTIPLY | PW_CURSOR_ORBIS_SWAP_RB) == 0xff0000ffu);
    pw_cursor_orbis_test_image_a(image);
    pw_cursor_orbis_convert(image, PW_CURSOR_ORBIS_SWAP_RB);
    assert(image[48 * 64 + 16] == 0xff0000ffu && image[16 * 64 + 16] == PW_CURSOR_ORBIS_WHITE);

    /* The place: 1:1 for the desktop at the swapchain's size, scaled from the top left otherwise. */
    pw_cursor_orbis_place(928, 508, 1920, 1080, 1920, 1080, &fx, &fy);
    assert(fx == 928 && fy == 508);
    pw_cursor_orbis_place(400, 300, 800, 600, 1920, 1080, &fx, &fy);
    assert(fx == 960 && fy == 540);
    pw_cursor_orbis_place(-5, -7, 1920, 1080, 1920, 1080, &fx, &fy);
    assert(fx == 0 && fy == 0);
    pw_cursor_orbis_place(10, 20, 1920, 1080, 0, 0, &fx, &fy);
    assert(fx == 10 && fy == 20);

    /* The handle: close-old then open-new, and open-new then close-old. */
    pw_cursor_orbis_opened(&current, 5);
    assert(current == 5);
    assert(pw_cursor_orbis_closed(&current, 5) == 1 && current == -1);
    pw_cursor_orbis_opened(&current, 6);
    pw_cursor_orbis_opened(&current, 7);
    assert(pw_cursor_orbis_closed(&current, 6) == 0 && current == 7);
    assert(pw_cursor_orbis_closed(&current, 7) == 1 && current == -1);
    pw_cursor_orbis_opened(&current, -2147024891);   /* a refused open leaves no handle */
    assert(current == -1 && pw_cursor_orbis_closed(&current, -1) == 0);

    /* Status text. */
    assert(!strcmp(pw_cursor_orbis_rc(0, rc, sizeof(rc)), "0"));
    assert(!strcmp(pw_cursor_orbis_rc((int)0x80290005u, rc, sizeof(rc)), "0x80290005"));
    assert(!strcmp(pw_cursor_orbis_rc(1, rc, sizeof(rc)), "0x1"));

    puts("test_pw_videoout_cursor_orbis: ok");
    return 0;
}
