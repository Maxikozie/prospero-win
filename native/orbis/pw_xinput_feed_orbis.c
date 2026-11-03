/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_xinput_feed_orbis.h"
#include <string.h>

void pw_xinput_feed_init(PwXinputFeed *feed)
{
    if (!feed) return;
    memset(feed, 0, sizeof(*feed));
    feed->lt_min = feed->rt_min = 0xff;
    feed->wake_period_us = PW_XINPUT_FEED_WAKE_US;
}

static size_t add_binding(const PwGameBinding *b, uint32_t down, PwWineInput *out, size_t used, size_t max)
{
    if (used >= max || (b->kind != PW_GAME_BIND_KEY && b->kind != PW_GAME_BIND_MOUSE)) return used;
    out[used].type = b->kind == PW_GAME_BIND_KEY ? PW_WINE_INPUT_KEY : PW_WINE_INPUT_MOUSE_BUTTON;
    out[used].code = b->code;
    out[used].x = out[used].y = 0;
    out[used].down = down;
    return used + 1;
}

size_t pw_xinput_feed_step(PwXinputFeed *feed, const PwGameInput *input, const PwPadPs5 *pad, uint64_t now_us,
                           PwWinePad *out, int *mark, PwWineInput *events, size_t max)
{
    uint32_t buttons = 0, pressed, released;
    unsigned readable, touch, left_now, right_now;
    size_t used = 0;

    if (mark) *mark = 0;
    if (!feed || !out) return 0;
    pw_wine_game_pad(pad, out);
    out->buttons &= (uint16_t)~0x0400u;   /* Guide: the right half is the title's */
    readable = pad && pad->core.connected && !pad->intercepted;
    if (readable) buttons = pad->core.previous_buttons;
    touch = (buttons & PW_PAD_PS5_TOUCHPAD) != 0;
    left_now = touch && pad->touch_left;
    right_now = touch && !pad->touch_left;
    buttons &= ~(uint32_t)PW_PAD_PS5_TOUCHPAD;
    if (readable) {
        if (pad->l2 < feed->lt_min) feed->lt_min = pad->l2;
        if (pad->r2 < feed->rt_min) feed->rt_min = pad->r2;
        feed->trigger_samples++;
        if (feed->right_held && !right_now) {
            feed->marks++;
            if (mark) *mark = 1;
        }
    }
    feed->right_held = right_now;
    pressed = buttons & ~feed->held;
    released = feed->held & ~buttons;
    feed->held = buttons;
    if (!events) {
        feed->left_held = left_now;
        return 0;
    }
    if (input) {
        /* Releases first, then presses, as pw_wine_game_inputs orders them. */
        if (feed->left_held && !left_now) used = add_binding(&input->touchpad_left, 0, events, used, max);
        used += pw_wine_game_inputs(input, 0, released, events + used, max - used);
        if (!feed->left_held && left_now) {
            size_t before = used;
            used = add_binding(&input->touchpad_left, 1, events, used, max);
            if (used > before) feed->left_downs++;
        }
        used += pw_wine_game_inputs(input, pressed, 0, events + used, max - used);
        feed->keys += used;
    }
    feed->left_held = left_now;
    if (feed->wake_period_us && used < max &&
        (!feed->woke || now_us - feed->last_wake_us >= feed->wake_period_us)) {
        events[used].type = PW_WINE_INPUT_MOUSE_MOVE;
        events[used].code = 0;
        events[used].x = events[used].y = 0;
        events[used].down = 0;
        used++;
        feed->woke = 1;
        feed->last_wake_us = now_us;
        feed->wakes++;
    }
    return used;
}
