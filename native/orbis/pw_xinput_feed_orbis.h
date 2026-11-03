/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_XINPUT_FEED_ORBIS_H
#define PW_XINPUT_FEED_ORBIS_H
/*
 * The Orbis title's XInput feed, one pad read at a time: the DS4 (read through
 * the PS5 pad core) becomes controller 0's state with pw_wine_game_pad's
 * mapping, and a few of its buttons may also post keys from the game
 * profile's bindings, beside the pad, as the PS5 title does.
 *
 * The touchpad is split as pw_wine_game_pad splits it. Its left half is Back
 * for the game and, when the bindings set touchpad_left, that key too. Its
 * right half is the title's mark: counted when released, never given to the
 * game. Every other button posts its bindings[] entry, if any (the touchpad's
 * own entry is the caller's to clear).
 *
 * Key edges follow the buttons the feed actually gave the game: while the
 * system holds the pad (intercepted), the pad is gone or its reads fail,
 * those count as released, so no key stays down.
 *
 * The wake: once a second the step adds a mouse move of (0,0). Wine's PS5
 * user driver makes the newest shown window the foreground only while it
 * processes posted input, and a game whose window was shown without
 * activation (DXVK's fullscreen SetWindowPos has SWP_NOACTIVATE) then never
 * becomes foreground when the pad posts no keys; a game that reads XInput only
 * while it is foreground (Psychonauts) then ignores the pad. The move is a
 * no-op for the driver after its first, which centres the pointer. It is the
 * feed's event: counted in wakes, never in keys.
 */
#include <stddef.h>
#include <stdint.h>
#include "../pw_pad_ps5.h"
#include "../pw_wine_display.h"

typedef struct PwXinputFeed {
    uint32_t held;              /* DS4 buttons as last fed, touchpad bit excluded */
    unsigned left_held, right_held;
    uint64_t marks;             /* right-half releases */
    uint64_t keys;              /* key and mouse-button events produced */
    uint64_t left_downs;        /* touchpad_left presses that produced its key */
    uint64_t trigger_samples;   /* reads that set lt_min/rt_min */
    uint8_t lt_min, rt_min;     /* the lowest L2/R2 seen while readable: a resting trigger */
    uint64_t wakes;             /* (0,0) moves produced */
    uint64_t last_wake_us;
    uint64_t wake_period_us;    /* PW_XINPUT_FEED_WAKE_US; 0: no wake */
    unsigned woke;
} PwXinputFeed;

enum { PW_XINPUT_FEED_WAKE_US = 1000000 };

void pw_xinput_feed_init(PwXinputFeed *feed);

/* After pw_pad_ps5_read(pad) at now_us (a monotonic clock): fills *out with
 * controller 0's state (connected 0: no controller) and events[] with up to
 * max events, the key edges first, then the wake when it is due; returns
 * their count. *mark is 1 when this read released the touchpad's right half.
 * input may be NULL (no key is posted; the wake still is). */
size_t pw_xinput_feed_step(PwXinputFeed *feed, const PwGameInput *input, const PwPadPs5 *pad, uint64_t now_us,
                           PwWinePad *out, int *mark, PwWineInput *events, size_t max);

#endif
