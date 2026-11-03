/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The DS4's sample through the PS5 pad core (native/orbis/pw_pad_ps4_orbis.c):
 * the connected byte with unknown padding after it, the touch block kept (a
 * click's half), the intercepted bit, a disconnect. */
#include "../native/orbis/pw_pad_ps4_orbis.h"
#include <assert.h>
#include <string.h>

enum { BUTTONS = 0, LX = 4, LY = 5, RX = 6, RY = 7, L2 = 8, R2 = 9, FINGERS = 52, TOUCH0_X = 60, TOUCH0_Y = 62,
       CONNECTED = 76, TIMESTAMP = 80, COUNT = 104 };

static uint8_t script[8][PW_PAD_PS4_SAMPLE_BYTES];
static int script_len, script_at;

static void sample(uint8_t *raw, uint32_t buttons, uint8_t connected, uint8_t fingers, uint16_t touch_x)
{
    uint64_t ts = 1000u + (uint64_t)script_len;
    memset(raw, 0, PW_PAD_PS4_SAMPLE_BYTES);
    memcpy(raw + BUTTONS, &buttons, 4);
    raw[LX] = 0x00; raw[LY] = 0xff; raw[RX] = 0xff; raw[RY] = 0x00;
    raw[L2] = 200; raw[R2] = 255;
    raw[FINGERS] = fingers;
    memcpy(raw + TOUCH0_X, &touch_x, 2);
    raw[CONNECTED] = connected;
    raw[CONNECTED + 1] = 0xa5; raw[CONNECTED + 2] = 0x5a; raw[CONNECTED + 3] = 0xff;   /* unknown padding */
    memcpy(raw + TIMESTAMP, &ts, 8);
    raw[COUNT] = 1;
}

static int user_initialize(const void *p) { (void)p; return 1; }
static int foreground_user(int32_t *u) { *u = 7; return 0; }
static int user_terminate(void) { assert(!"the title's user service is never terminated"); return 0; }
static int pad_init(void) { return 0; }
static int pad_open(int32_t u, int32_t t, int32_t i, const void *p) { (void)u; (void)t; (void)i; (void)p; return 3; }
static int pad_close(int32_t h) { (void)h; return 0; }
static int pad_read(int32_t h, PwPadPs5Data *out, int32_t n)
{
    (void)h;
    if (script_at >= script_len || n < 1) return 0;
    pw_pad_ps4_convert(script[script_at++], out);
    return 1;
}

int main(void)
{
    PwPadPs5Ops ops = { user_initialize, foreground_user, user_terminate, pad_init, pad_open, pad_read, pad_close,
                        NULL, NULL, NULL, NULL };
    static const PwPadKeyMap map[] = { { 0x4000u, 0x20, 0, 0, "cross" } };
    PwPadPs5Data data;
    PwPadPs5 pad;

    /* The conversion: everything up to the touch block as it came, connected one byte. */
    sample(script[0], 0x4000u, 1, 1, 300);
    pw_pad_ps4_convert(script[0], &data);
    assert(data.buttons == 0x4000u && data.connected == 1 && data.left_stick.x == 0 && data.left_stick.y == 0xff);
    assert(data.l2 == 200 && data.r2 == 255 && data.touch_data.fingers == 1 && data.touch_data.touch[0].x == 300);
    assert(data.timestamp == 1000u && data.connected_count == 1);
    script[0][CONNECTED] = 0;
    pw_pad_ps4_convert(script[0], &data);
    assert(data.connected == 0);

    /* Through the core: a click on the left half, released; the right half; intercepted; unplugged. */
    script_len = 0;
    sample(script[script_len++], 0x100000u | 0x4000u, 1, 1, 300);
    sample(script[script_len++], 0x4000u, 1, 0, 0);
    sample(script[script_len++], 0x100000u, 1, 1, 1500);
    sample(script[script_len++], 0x80000000u | 0x4000u, 1, 0, 0);
    sample(script[script_len++], 0x4000u, 0, 0, 0);
    script_at = 0;
    assert(pw_pad_ps5_open(&pad, &ops, map, 1) == PW_OK && pad.opened && pad.pad_handle == 3 && pad.user_id == 7);
    assert(!pad.owns_user_service);
    assert(pw_pad_ps5_read(&pad) == PW_OK);
    assert(pad.core.connected && (pad.core.previous_buttons & 0x100000u) && pad.touch_left);
    assert(pad.left_stick.x == 0 && pad.l2 == 200);
    assert(pw_pad_ps5_read(&pad) == PW_OK && !(pad.core.previous_buttons & 0x100000u));
    assert(pw_pad_ps5_read(&pad) == PW_OK && (pad.core.previous_buttons & 0x100000u) && !pad.touch_left);
    assert(pw_pad_ps5_read(&pad) == PW_OK && pad.intercepted && !pad.core.connected && pad.left_stick.x == 0x80);
    assert(pw_pad_ps5_read(&pad) == PW_OK && !pad.intercepted && !pad.core.connected && pad.disconnected_samples == 1);
    assert(pw_pad_ps5_close(&pad) == PW_OK);
    return 0;
}
