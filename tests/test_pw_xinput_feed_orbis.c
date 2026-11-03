/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The Orbis XInput feed's step (native/orbis/pw_xinput_feed_orbis.c), from the
 * DS4's raw sample through the PS5 pad core: controller 0's state, the
 * touchpad's halves (left: Back plus touchpad_left's key, right: the mark),
 * key edges that never stay down, the triggers' resting values, and the
 * wake: a (0,0) mouse move once a second, which is no key and no button. */
#include "../native/orbis/pw_xinput_feed_orbis.h"
#include "../native/orbis/pw_pad_ps4_orbis.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { BUTTONS = 0, LX = 4, LY = 5, RX = 6, RY = 7, L2 = 8, R2 = 9, FINGERS = 52, TOUCH0_X = 60,
       CONNECTED = 76, TIMESTAMP = 80, COUNT = 104 };
enum { CROSS = 0x4000u, SQUARE = 0x8000u, OPTIONS = 0x8u, TOUCH = 0x100000u, INTERCEPTED = 0x80000000u };
enum { XI_A = 0x1000u, XI_X = 0x4000u, XI_START = 0x0010u, XI_BACK = 0x0020u, XI_GUIDE = 0x0400u };

static uint8_t raw_sample[PW_PAD_PS4_SAMPLE_BYTES];
static int read_rc = 1;

static void set_sample(uint32_t buttons, uint8_t connected, uint16_t touch_x, uint8_t l2, uint8_t r2)
{
    static uint64_t ts = 1000;
    memset(raw_sample, 0, sizeof(raw_sample));
    memcpy(raw_sample + BUTTONS, &buttons, 4);
    raw_sample[LX] = raw_sample[LY] = raw_sample[RX] = raw_sample[RY] = 0x80;
    raw_sample[L2] = l2;
    raw_sample[R2] = r2;
    raw_sample[FINGERS] = (buttons & TOUCH) ? 1 : 0;
    memcpy(raw_sample + TOUCH0_X, &touch_x, 2);
    raw_sample[CONNECTED] = connected;
    ts++;
    memcpy(raw_sample + TIMESTAMP, &ts, 8);
    raw_sample[COUNT] = 1;
    read_rc = 1;
}

static int user_initialize(const void *p) { (void)p; return 1; }
static int foreground_user(int32_t *u) { *u = 7; return 0; }
static int user_terminate(void) { return 0; }
static int pad_init(void) { return 0; }
static int pad_open(int32_t u, int32_t t, int32_t i, const void *p) { (void)u; (void)t; (void)i; (void)p; return 3; }
static int pad_close(int32_t h) { (void)h; return 0; }
static int pad_read(int32_t h, PwPadPs5Data *out, int32_t n)
{
    (void)h;
    if (read_rc <= 0 || n < 1) return read_rc;
    pw_pad_ps4_convert(raw_sample, out);
    return 1;
}

static PwPadPs5 pad;
static PwXinputFeed feed;
static PwWinePad state;
static PwWineInput events[8];
static int mark;
static uint64_t now_us;

/* One read 4 ms after the last, then the step; returns the events' count. */
static size_t step(const PwGameInput *input)
{
    pw_pad_ps5_read(&pad);
    now_us += 4000;
    return pw_xinput_feed_step(&feed, input, &pad, now_us, &state, &mark, events, 8);
}

static int is_wake(size_t i)
{
    return events[i].type == PW_WINE_INPUT_MOUSE_MOVE && !events[i].x && !events[i].y && !events[i].code &&
           !events[i].down;
}

static int is_key(size_t i, uint32_t code, uint32_t down)
{
    return events[i].type == PW_WINE_INPUT_KEY && events[i].code == code && events[i].down == down;
}

static void open_pad(void)
{
    static const PwPadKeyMap map[] = { { 0x4000u, 0x20, 0, 0, "cross" } };
    PwPadPs5Ops ops = { user_initialize, foreground_user, user_terminate, pad_init, pad_open, pad_read, pad_close,
                        NULL, NULL, NULL, NULL };
    memset(&pad, 0, sizeof(pad));
    set_sample(0, 1, 0, 0, 0);
    read_rc = 0;
    assert(pw_pad_ps5_open(&pad, &ops, map, 1) == PW_OK);
    pw_xinput_feed_init(&feed);
    assert(feed.wake_period_us == PW_XINPUT_FEED_WAKE_US);
    feed.wake_period_us = 0;   /* the cases below count key edges alone; the wake has its own */
}

int main(void)
{
    static const char preset[] = "[input]\nmode = xinput\ntouchpad_left = enter\n";
    PwGameInput input;
    size_t n;

    pw_game_input_init(&input);
    assert(pw_game_input_parse((const uint8_t *)preset, sizeof(preset) - 1, &input) == PW_OK);
    input.bindings[16].kind = PW_GAME_BIND_NONE;   /* the touchpad's own entry, as the title clears it */

    /* The left half: Back for the game and Enter, once down, once up. */
    open_pad();
    set_sample(0, 1, 0, 0, 0);
    assert(step(&input) == 0 && state.connected && state.buttons == 0);
    set_sample(TOUCH, 1, 300, 0, 0);
    n = step(&input);
    assert(n == 1 && is_key(0, 0x0d, 1) && state.buttons == XI_BACK && !mark);
    assert(step(&input) == 0 && state.buttons == XI_BACK);   /* held: no repeat */
    set_sample(0, 1, 0, 0, 0);
    n = step(&input);
    assert(n == 1 && is_key(0, 0x0d, 0) && state.buttons == 0 && !mark);
    assert(feed.left_downs == 1 && feed.keys == 2 && feed.marks == 0);

    /* The right half: no button for the game, no key, a mark on release. */
    set_sample(TOUCH, 1, 1500, 0, 0);
    assert(step(&input) == 0 && state.buttons == 0 && !mark);
    set_sample(0, 1, 0, 0, 0);
    assert(step(&input) == 0 && mark && feed.marks == 1);

    /* Cross is A, Options Start, Square X: pad buttons only, no key. */
    set_sample(CROSS | OPTIONS | SQUARE, 1, 0, 0, 0);
    assert(step(&input) == 0 && state.buttons == (XI_A | XI_START | XI_X));
    set_sample(0, 1, 0, 0, 0);
    assert(step(&input) == 0 && state.buttons == 0);

    /* Intercepted while the left half is held: the slot idles, Enter goes up. */
    set_sample(TOUCH, 1, 300, 0, 0);
    assert(step(&input) == 1 && is_key(0, 0x0d, 1));
    set_sample(INTERCEPTED | TOUCH, 1, 300, 0, 0);
    n = step(&input);
    assert(n == 1 && is_key(0, 0x0d, 0) && state.connected && state.buttons == 0);
    set_sample(0, 1, 0, 0, 0);
    assert(step(&input) == 0);

    /* A read error while held: no controller, Enter up. */
    set_sample(TOUCH, 1, 300, 0, 0);
    assert(step(&input) == 1 && is_key(0, 0x0d, 1));
    read_rc = -1;
    n = step(&input);
    assert(n == 1 && is_key(0, 0x0d, 0) && !state.connected);
    /* Unplugged while held: the same. */
    set_sample(TOUCH, 1, 300, 0, 0);
    assert(step(&input) == 1 && is_key(0, 0x0d, 1));
    set_sample(TOUCH, 0, 300, 0, 0);
    n = step(&input);
    assert(n == 1 && is_key(0, 0x0d, 0) && !state.connected);
    /* Interception while the right half is held is no mark. */
    set_sample(TOUCH, 1, 1500, 0, 0);
    step(&input);
    set_sample(INTERCEPTED, 1, 0, 0, 0);
    assert(step(&input) == 0 && !mark);

    /* No bindings (a profile that binds nothing, xinput32.ini): nothing posted, the same state. */
    {
        PwGameInput none;
        pw_game_input_init(&none);
        open_pad();
        set_sample(TOUCH | CROSS | OPTIONS, 1, 300, 0, 0);
        assert(step(&none) == 0 && state.buttons == (XI_BACK | XI_A | XI_START));
        set_sample(0, 1, 0, 0, 0);
        assert(step(&none) == 0 && feed.keys == 0);
        set_sample(TOUCH, 1, 300, 0, 0);
        assert(pw_pad_ps5_read(&pad) == PW_OK);
        assert(pw_xinput_feed_step(&feed, NULL, &pad, now_us, &state, &mark, events, 8) == 0 && state.buttons == XI_BACK);
    }

    /* The triggers' resting values: the lowest while readable; an intercepted read (released, 0) does not count. */
    open_pad();
    set_sample(0, 1, 0, 2, 0);
    step(&input);
    set_sample(0, 1, 0, 255, 200);
    step(&input);
    assert(feed.lt_min == 2 && feed.rt_min == 0 && feed.trigger_samples == 2);
    open_pad();
    set_sample(0, 1, 0, 3, 4);
    step(&input);
    set_sample(INTERCEPTED, 1, 0, 0, 0);
    step(&input);
    assert(feed.lt_min == 3 && feed.rt_min == 4 && feed.trigger_samples == 1);
    pw_xinput_feed_init(&feed);
    assert(feed.lt_min == 0xff && feed.rt_min == 0xff && !feed.trigger_samples);

    /* The wake: a (0,0) move at the first step, then once a second, whatever the pad does. Over
     * 2.5 s of an idle pad the feed produces three wakes and nothing else: no key, no button, no
     * motion. kbm in milestone l's line counts the USB keyboard's and mouse's events (g_script's
     * HID poll) and nothing the feed posts, so it stays 0 with the wake running. */
    {
        size_t total = 0, wakes = 0;
        open_pad();
        feed.wake_period_us = PW_XINPUT_FEED_WAKE_US;
        now_us = 5000000;
        set_sample(0, 1, 0, 0, 0);
        for (int i = 0; i < 625; i++) {   /* 625 reads, 4 ms apart: 2.5 s */
            n = step(&input);
            for (size_t k = 0; k < n; k++) {
                assert(is_wake(k));
                wakes++;
            }
            total += n;
        }
        assert(total == 3 && wakes == 3 && feed.wakes == 3 && feed.keys == 0 && feed.left_downs == 0);
        assert(state.connected && state.buttons == 0);

        /* A key and a due wake in one read: the key first. Keys and wakes are counted apart. */
        now_us += PW_XINPUT_FEED_WAKE_US;
        set_sample(TOUCH, 1, 300, 0, 0);
        n = step(&input);
        assert(n == 2 && is_key(0, 0x0d, 1) && is_wake(1) && feed.keys == 1 && feed.wakes == 4);
        set_sample(0, 1, 0, 0, 0);
        n = step(&input);
        assert(n == 1 && is_key(0, 0x0d, 0) && feed.keys == 2 && feed.wakes == 4);

        /* No bindings (l1's profile): the wake alone. No room for an event: no wake. The pad
         * unread or the system holding it: the wake goes on (the foreground is not the pad's). */
        now_us += PW_XINPUT_FEED_WAKE_US;
        pw_pad_ps5_read(&pad);
        assert(pw_xinput_feed_step(&feed, NULL, &pad, now_us, &state, &mark, events, 8) == 1 && is_wake(0));
        now_us += PW_XINPUT_FEED_WAKE_US;
        assert(pw_xinput_feed_step(&feed, &input, &pad, now_us, &state, &mark, events, 0) == 0 && feed.wakes == 5);
        set_sample(INTERCEPTED, 1, 0, 0, 0);
        n = step(&input);
        assert(n == 1 && is_wake(0) && feed.wakes == 6);
    }

    /* Without a feed or an output, nothing. */
    assert(pw_xinput_feed_step(NULL, &input, &pad, now_us, &state, &mark, events, 8) == 0 && !mark);
    assert(pw_xinput_feed_step(&feed, &input, &pad, now_us, NULL, &mark, events, 8) == 0);

    printf("xinput feed passed: the touchpad's halves (Back plus its key, the mark), pad buttons without keys, keys "
           "released on interception, read errors and unplugging, no keys without bindings, the triggers' rest, the wake once a second (no key, no button, no motion)\n");
    return 0;
}
