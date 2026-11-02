/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_wine_display.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static void test_frames(void)
{
    PwWineFrameBox box;
    PwPresentView view;
    uint8_t source[3 * 16], out[64], small[8], storage[48];
    uint64_t seen = 0;

    for (size_t i = 0; i < sizeof(source); i++) source[i] = (uint8_t)i;
    assert(pw_wine_frame_box_init(NULL, storage, 48) == -1);
    assert(pw_wine_frame_box_init(&box, NULL, 48) == -1 && pw_wine_frame_box_init(&box, storage, 0) == -1);
    assert(!pw_wine_frame_box_init(&box, storage, sizeof(storage)));
    assert(pw_wine_frame_box_take(&box, &seen, out, sizeof(out), &view) == 0);

    /* Rows are packed: a 3x2 frame with a 16-byte stride keeps 12 bytes a row. */
    assert(!pw_wine_frame_box_put(&box, source, 3, 2, 16));
    assert(pw_wine_frame_box_take(&box, &seen, small, sizeof(small), &view) == -1 && seen == 0);
    assert(pw_wine_frame_box_take(&box, &seen, out, sizeof(out), &view) == 1 && seen == 1);
    assert(view.pixels == out && view.width == 3 && view.height == 2 && view.stride == 12 &&
           view.bytes == 24);
    assert(!memcmp(out, source, 12) && !memcmp(out + 12, source + 16, 12));
    assert(pw_wine_frame_box_take(&box, &seen, out, sizeof(out), &view) == 0);

    /* Only the newest of several frames is taken. */
    assert(!pw_wine_frame_box_put(&box, source, 1, 1, 4));
    assert(!pw_wine_frame_box_put(&box, source + 4, 2, 1, 8));
    assert(pw_wine_frame_box_take(&box, &seen, out, sizeof(out), &view) == 1 && seen == 3);
    assert(view.width == 2 && view.height == 1 && !memcmp(out, source + 4, 8));

    /* Refusals: invalid, stride below a row, larger than the box. */
    assert(pw_wine_frame_box_put(&box, NULL, 1, 1, 4) == -1);
    assert(pw_wine_frame_box_put(&box, source, 0, 1, 4) == -1);
    assert(pw_wine_frame_box_put(&box, source, 1, 0, 4) == -1);
    assert(pw_wine_frame_box_put(&box, source, 2, 1, 4) == -1);
    assert(pw_wine_frame_box_put(&box, source, 4, 4, 16) == -1);
    assert(pw_wine_frame_box_put(&box, source, 0x40000000u, 2, 0xffffffffu) == -1);
    assert(box.rejected == 6 && box.sequence == 3);
    assert(pw_wine_frame_box_put(NULL, source, 1, 1, 4) == -1);
    assert(pw_wine_frame_box_take(&box, NULL, out, sizeof(out), &view) == -1);
    assert(pw_wine_frame_box_take(&box, &seen, NULL, sizeof(out), &view) == -1);
    assert(pw_wine_frame_box_take(&box, &seen, out, sizeof(out), NULL) == -1);
    pw_wine_frame_box_destroy(&box);
    pw_wine_frame_box_destroy(&box);              /* a second destroy is harmless */
    pw_wine_frame_box_destroy(NULL);
    assert(pw_wine_frame_box_put(&box, source, 1, 1, 4) == -1);
    assert(pw_wine_frame_box_take(&box, &seen, out, sizeof(out), &view) == -1);
}

/* Wine's threads put while the title's thread takes. */
static PwWineFrameBox shared;
static uint8_t shared_storage[16];

static void *producer(void *arg)
{
    uint32_t pixels[4];

    (void)arg;
    for (uint32_t i = 1; i <= 2000; i++) {
        for (int p = 0; p < 4; p++) pixels[p] = i;
        assert(!pw_wine_frame_box_put(&shared, pixels, 2, 2, 8));
    }
    return NULL;
}

static void test_threads(void)
{
    pthread_t thread;
    PwPresentView view;
    uint32_t out[4], last = 0;
    uint64_t seen = 0;

    assert(!pw_wine_frame_box_init(&shared, shared_storage, sizeof(shared_storage)));
    assert(!pthread_create(&thread, NULL, producer, NULL));
    while (last < 2000) {
        if (pw_wine_frame_box_take(&shared, &seen, (uint8_t *)out, sizeof(out), &view) != 1)
            continue;
        /* never a torn frame, never older than the last one taken */
        assert(out[0] == out[1] && out[1] == out[2] && out[2] == out[3] && out[0] >= last);
        last = out[0];
    }
    assert(!pthread_join(thread, NULL) && seen == 2000);
    pw_wine_frame_box_destroy(&shared);
}

static const char preset_text[] =
    "[input]\nl1 = z\nr1 = slash\ncross = space\nr2 = mouse_left\nl2 = mouse_right\n"
    "square = none\n";
enum { L1 = 0x400u, R1 = 0x800u, CROSS = 0x4000u, SQUARE = 0x8000u, L2 = 0x100u, R2 = 0x200u,
       CIRCLE = 0x2000u };

static void test_inputs(void)
{
    PwGameInput input;
    PwWineInput events[8];

    pw_game_input_init(&input);
    assert(pw_game_input_parse((const uint8_t *)preset_text, sizeof(preset_text) - 1, &input) == PW_OK);
    assert(pw_wine_game_inputs(&input, L1 | CROSS | CIRCLE | SQUARE, 0, events, 8) == 2);
    assert(events[0].type == PW_WINE_INPUT_KEY && events[0].code == 0x20 && events[0].down == 1);
    assert(events[1].code == 'Z' && events[1].down == 1 && !events[1].x && !events[1].y);

    /* Releases come first; mouse bindings send buttons; unbound and none send nothing. */
    assert(pw_wine_game_inputs(&input, R2 | L2, L1 | CROSS, events, 8) == 4);
    assert(events[0].code == 0x20 && !events[0].down && events[1].code == 'Z' && !events[1].down);
    assert(events[2].type == PW_WINE_INPUT_MOUSE_BUTTON && events[2].code == 1 && events[2].down);
    assert(events[3].type == PW_WINE_INPUT_MOUSE_BUTTON && events[3].code == 0 && events[3].down);
    assert(pw_wine_game_inputs(&input, R1 | L1, 0, events, 1) == 1 && events[0].code == 'Z');
    assert(pw_wine_game_inputs(&input, 0, 0, events, 8) == 0);
    assert(pw_wine_game_inputs(NULL, L1, 0, events, 8) == 0 && pw_wine_game_inputs(&input, L1, 0, NULL, 8) == 0);
}

static void test_pointer(void)
{
    PwWinePointer pointer = { 0, 0 };
    PwWineInput move;

    /* At rest and inside the dead zone nothing moves. */
    assert(!pw_wine_pointer_step(&pointer, 0x80, 0x80, 1200, 16000, &move));
    assert(!pw_wine_pointer_step(&pointer, 0x80 + PW_WINE_POINTER_DEADZONE, 0x80 - PW_WINE_POINTER_DEADZONE,
                                 1200, 1000000, &move));
    assert(!pointer.x && !pointer.y);
    /* Full tilt for a second is the speed's worth of motion, right or up. */
    assert(pw_wine_pointer_step(&pointer, 0xff, 0x80, 300, 1000000, &move) == 1);
    assert(move.type == PW_WINE_INPUT_MOUSE_MOVE && move.x == 300 && move.y == 0 && !move.code && !move.down);
    assert(pw_wine_pointer_step(&pointer, 0x80, 0x00, 1200, 1000000, &move) == 1);
    assert(move.x == 0 && move.y == -1200);
    /* Motion has no edge: held, it keeps coming, which is what scrolls a
     * strategy game's map with the pointer against the screen's edge. */
    for (int i = 0; i < 3; i++) {
        assert(pw_wine_pointer_step(&pointer, 0xff, 0xff, 20000, 1000000, &move) == 1);
        assert(move.x == 20000 && move.y == 20000);
    }
    /* Half tilt is slower than half speed (squared curve). */
    assert(pw_wine_pointer_step(&pointer, 0x80 + 74, 0x80, 1000, 1000000, &move) == 1);
    assert(move.x > 200 && move.x < 300 && move.y == 0);
}

static void test_pointer_fraction(void)
{
    PwWinePointer pointer = { 0, 0 };
    PwWineInput move;
    int32_t right = 0, left = 0;
    int moves = 0;

    /* Steps smaller than a pixel add up: 60 px/s for 100 frames of 16 ms is
     * 62914/65536 of a pixel a step: 95 whole pixels, sent a pixel at a time,
     * with the fraction kept. */
    for (int i = 0; i < 100; i++)
        if (pw_wine_pointer_step(&pointer, 0xff, 0x80, 60, 16000, &move)) {
            assert(move.x == 1 && move.y == 0);
            right += move.x;
            moves++;
        }
    assert(right == 95 && moves == 95 && pointer.x > 0 && pointer.x < 65536);
    /* Left is the mirror image: the fraction rounds towards zero both ways. */
    pointer = (PwWinePointer){ 0, 0 };
    for (int i = 0; i < 100; i++)
        if (pw_wine_pointer_step(&pointer, 0x01, 0x80, 60, 16000, &move)) left += move.x;
    assert(left == -95 && pointer.x < 0 && pointer.x > -65536);
    assert(!pw_wine_pointer_step(NULL, 0xff, 0x80, 1, 1, &move) && !pw_wine_pointer_step(&pointer, 0, 0, 1, 1, NULL));
}

static void test_pad(void)
{
    PwPadPs5 pad;
    PwWinePad xinput;

    memset(&pad, 0, sizeof(pad));
    pad.left_stick = pad.right_stick = (PwPadPs5Stick){ 0x80, 0x80 };
    /* Disconnected: nothing, and a neutral state. */
    memset(&xinput, 0x5a, sizeof(xinput));
    assert(pw_wine_game_pad(&pad, &xinput) == 0);
    assert(!xinput.connected && !xinput.buttons && !xinput.thumb_lx && !xinput.left_trigger);
    assert(pw_wine_game_pad(NULL, &xinput) == 0 && pw_wine_game_pad(&pad, NULL) == 0);

    /* The system has it (intercepted): still connected, idle, whatever
     * the last buttons and sticks were. */
    pad.intercepted = 1;
    pad.core.previous_buttons = 0x4000u | 0x100000u;
    pad.left_stick = (PwPadPs5Stick){ 0, 0 };
    pad.l2 = 200;
    assert(pw_wine_game_pad(&pad, &xinput) == 1 && xinput.connected == 1);
    assert(!xinput.buttons && !xinput.thumb_lx && !xinput.thumb_ly && !xinput.left_trigger);
    pad.intercepted = 0;
    pad.core.previous_buttons = 0;
    pad.left_stick = (PwPadPs5Stick){ 0x80, 0x80 };
    pad.l2 = 0;
    assert(pw_wine_game_pad(&pad, &xinput) == 0 && !xinput.connected);

    /* Centred and released. */
    pad.core.connected = 1;
    assert(pw_wine_game_pad(&pad, &xinput) == 1 && xinput.connected == 1 && xinput.packet == 0);
    assert(!xinput.buttons && !xinput.left_trigger && !xinput.right_trigger);
    assert(!xinput.thumb_lx && !xinput.thumb_ly && !xinput.thumb_rx && !xinput.thumb_ry);

    /* Each DualSense button, alone, as its XInput bit. */
    static const struct { uint32_t dualsense; uint16_t xinput; } buttons[] = {
        { 0x4000u, 0x1000u /* cross: A */ }, { 0x2000u, 0x2000u /* circle: B */ },
        { 0x8000u, 0x4000u /* square: X */ }, { 0x1000u, 0x8000u /* triangle: Y */ },
        { 0x400u, 0x0100u /* l1 */ }, { 0x800u, 0x0200u /* r1 */ },
        { 0x2u, 0x0040u /* l3 */ }, { 0x4u, 0x0080u /* r3 */ },
        { 0x10u, 0x0001u /* up */ }, { 0x40u, 0x0002u /* down */ },
        { 0x80u, 0x0004u /* left */ }, { 0x20u, 0x0008u /* right */ },
        { 0x8u, 0x0010u /* options: start */ }, { 0x1u, 0x0020u /* create: back */ },
        { 0x100000u, 0x0400u /* touchpad, right half: guide */ },
    };
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        pad.core.previous_buttons = buttons[i].dualsense;
        assert(pw_wine_game_pad(&pad, &xinput) == 1 && xinput.buttons == buttons[i].xinput);
    }
    /* The touchpad's left half is Back, beside Create's or alone. */
    pad.touch_left = 1;
    pad.core.previous_buttons = 0x100000u;
    assert(pw_wine_game_pad(&pad, &xinput) == 1 && xinput.buttons == 0x0020u);
    pad.core.previous_buttons = 0x100000u | 0x1u | 0x8u;
    assert(pw_wine_game_pad(&pad, &xinput) == 1 && xinput.buttons == (0x0020u | 0x0010u));
    pad.touch_left = 0;
    pad.core.previous_buttons = 0x100000u | 0x1u;
    assert(pw_wine_game_pad(&pad, &xinput) == 1 && xinput.buttons == (0x0400u | 0x0020u));
    pad.touch_left = 1;
    pad.core.previous_buttons = 0;
    assert(pw_wine_game_pad(&pad, &xinput) == 1 && !xinput.buttons);
    pad.touch_left = 0;
    /* L2/R2 are analog triggers, not buttons; unknown bits are dropped. */
    pad.core.previous_buttons = 0x100u | 0x200u | 0x80000000u;
    pad.l2 = 1;
    pad.r2 = 255;
    assert(pw_wine_game_pad(&pad, &xinput) == 1 && !xinput.buttons);
    assert(xinput.left_trigger == 1 && xinput.right_trigger == 255);
    pad.core.previous_buttons = 0x4000u | 0x8u | 0x10u;
    assert(pw_wine_game_pad(&pad, &xinput) == 1 && xinput.buttons == (0x1000u | 0x0010u | 0x0001u));

    /* Sticks: full reach both ways, centre 0, y up. */
    pad.left_stick = (PwPadPs5Stick){ 0x00, 0x00 };     /* left, up */
    pad.right_stick = (PwPadPs5Stick){ 0xff, 0xff };    /* right, down */
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(xinput.thumb_lx == -32768 && xinput.thumb_ly == 32767);
    assert(xinput.thumb_rx == 32767 && xinput.thumb_ry == -32768);
    pad.left_stick = (PwPadPs5Stick){ 0x81, 0x7f };     /* a step right, a step up */
    pad.right_stick = (PwPadPs5Stick){ 0x7f, 0x81 };
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(xinput.thumb_lx == 32767 / 127 && xinput.thumb_ly == 32767 / 128);
    assert(xinput.thumb_rx == -32768 / 128 && xinput.thumb_ry == -32768 / 127);
    pad.left_stick = (PwPadPs5Stick){ 0xc0, 0x40 };     /* halfway right and up */
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(xinput.thumb_lx == 64 * 32767 / 127 && xinput.thumb_ly == 64 * 32767 / 128);
}

/* A script build's controller added to the DualSense's. */
static void test_script_pad(void)
{
    PwPadPs5 pad;
    PwWinePad xinput;
    PwScriptPad script = { 0 };

    memset(&pad, 0, sizeof(pad));
    pad.core.connected = 1;
    pad.core.previous_buttons = 0x2000u;                /* circle: B */
    pad.left_stick = (PwPadPs5Stick){ 0xff, 0x80 };     /* full right */
    pad.right_stick = (PwPadPs5Stick){ 0x80, 0x00 };    /* full up */
    pad.r2 = 200;

    /* Nothing scripted: the real controller as it is. */
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(pw_wine_script_pad(&xinput, 1, &script, 1) == 1);
    assert(xinput.buttons == 0x2000u && xinput.thumb_lx == 32767 && xinput.thumb_ly == 0 &&
           xinput.thumb_ry == 32767 && xinput.right_trigger == 200);

    /* Buttons join; a stick held off centre replaces the real one, and
     * only that stick. */
    script.buttons = 0x1000u | 0x2000u;                 /* A, and B both ways */
    script.stick[0][0] = -16000;
    script.stick[0][1] = 0;
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(pw_wine_script_pad(&xinput, 1, &script, 1) == 1);
    assert(xinput.connected == 1 && xinput.buttons == 0x3000u);
    assert(xinput.thumb_lx == -16000 && xinput.thumb_ly == 0);
    assert(xinput.thumb_rx == 0 && xinput.thumb_ry == 32767 && xinput.right_trigger == 200);
    script.stick[0][0] = 0;
    script.stick[1][1] = -1;                            /* any off-centre value counts */
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(pw_wine_script_pad(&xinput, 1, &script, 1) == 1);
    assert(xinput.thumb_lx == 32767 && xinput.thumb_rx == 0 && xinput.thumb_ry == -1);

    /* No DualSense: the script's controller alone, still connected while it
     * holds nothing. */
    pad.core.connected = 0;
    memset(&script, 0, sizeof(script));
    assert(pw_wine_game_pad(&pad, &xinput) == 0);
    assert(pw_wine_script_pad(&xinput, 0, &script, 1) == 1);
    assert(xinput.connected == 1 && !xinput.buttons && !xinput.thumb_lx && !xinput.right_trigger);
    script.buttons = 0x0010u;
    memset(&xinput, 0x5a, sizeof(xinput));              /* stale: cleared when not connected */
    assert(pw_wine_script_pad(&xinput, 0, &script, 1) == 1);
    assert(xinput.buttons == 0x0010u && !xinput.thumb_lx && !xinput.left_trigger && !xinput.packet);

    /* A macro without pad events leaves controller 0 to the DualSense. */
    assert(pw_wine_script_pad(&xinput, 0, &script, 0) == 0 && !xinput.connected && !xinput.buttons);
    pad.core.connected = 1;
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(pw_wine_script_pad(&xinput, 1, &script, 0) == 1 && xinput.buttons == 0x2000u);
    assert(pw_wine_script_pad(&xinput, 1, NULL, 1) == 1 && xinput.buttons == 0x2000u);
    assert(pw_wine_script_pad(NULL, 1, &script, 1) == 0);

    /* From a parsed macro, through the scripted controller, to XInput. */
    {
        static PwScriptInputEvent storage[4];
        static const char macro[] = "0 pad a 1\n0 stick l 0 32767\n";
        PwScriptInput in;
        size_t bad;
        uint16_t changed = 0;

        memset(&script, 0, sizeof(script));
        assert(pw_script_input_parse(macro, sizeof(macro) - 1, storage, 4, &in, &bad) == PW_OK);
        for (size_t i = 0; i < in.count; i++) assert(pw_script_pad_apply(&script, &in.events[i], &changed) == 1);
        pad.core.connected = 0;
        assert(pw_wine_game_pad(&pad, &xinput) == 0);
        assert(pw_wine_script_pad(&xinput, 0, &script, in.pad_events != 0) == 1);
        assert(xinput.buttons == 0x1000u && xinput.thumb_lx == 0 && xinput.thumb_ly == 32767);
    }
}


/* keyboard_stick: one stick drives the four direction keys (upstream
 * prospero-win #736, 58b6e0c). Deadzone 25 of 127 counts gives a threshold
 * of 31, with five counts of hysteresis once a direction is held, and a
 * release is emitted before the opposite press. */
static void test_keyboard_stick(void)
{
    PwWineKeyboardStick state = { 0 };
    PwWineInput out[4];
    PwGameInput in;

    pw_game_input_init(&in);
    in.keyboard_stick = PW_GAME_STICK_LEFT;
    in.stick_deadzone = 25;                       /* threshold 31 */

    assert(pw_wine_keyboard_stick_step(&in, 0x80, 0x80, 1, &state, out, 4) == 0);
    assert(state.held == 0);
    assert(pw_wine_keyboard_stick_step(&in, 0x80, 128 - 31, 1, &state, out, 4) == 0);
    assert(pw_wine_keyboard_stick_step(&in, 0x80, 0x00, 1, &state, out, 4) == 1);
    assert(out[0].type == PW_WINE_INPUT_KEY && out[0].code == 'W' && out[0].down == 1);
    assert(pw_wine_keyboard_stick_step(&in, 0x80, 128 - 28, 1, &state, out, 4) == 0);
    assert(state.held != 0);
    assert(pw_wine_keyboard_stick_step(&in, 0x80, 0xff, 1, &state, out, 4) == 2);
    assert(out[0].code == 'W' && out[0].down == 0);
    assert(out[1].code == 'S' && out[1].down == 1);
    assert(pw_wine_keyboard_stick_step(&in, 0x80, 0xff, 0, &state, out, 4) == 1);
    assert(out[0].code == 'S' && out[0].down == 0 && state.held == 0);
    in.keyboard_stick = PW_GAME_STICK_NONE;
    assert(pw_wine_keyboard_stick_step(&in, 0x80, 0x00, 1, &state, out, 4) == 0);
    in.keyboard_stick = PW_GAME_STICK_LEFT;
    assert(pw_wine_keyboard_stick_step(&in, 0x00, 0x00, 1, &state, out, 4) == 2);
    assert(!pw_wine_keyboard_stick_step(NULL, 0, 0, 1, &state, out, 4));
    assert(!pw_wine_keyboard_stick_step(&in, 0, 0, 1, NULL, out, 4));
    assert(!pw_wine_keyboard_stick_step(&in, 0, 0, 1, &state, NULL, 4));
}

/* ---- the pad mapper ---------------------------------------------------------- */

static const char BLOODLINES[] =
    "[input]\nmode = keyboard\nmouse = right_stick\nkeyboard_stick = left_stick\nstick_walk = shift\nstick_run = 80\n"
    "layer = l1\nl1 = none\nr2 = mouse_left\nl2 = tab\nr1 = mouse_right\ncross = e\ntriangle = space\n"
    "square = r\nsquare.hold = h\ncircle = i\ncircle.hold = c\nl3 = toggle:ctrl\nr3 = z\nup = f\ndown = t\n"
    "left = rbracket\nright = lbracket\noptions = escape\n"
    "l1+cross = 1\nl1+circle = 2\nl1+square = 3\nl1+triangle = 4\nl1+up = 5\nl1+right = 6\nl1+down = 7\n"
    "l1+left = 8\nl1+r1 = 9\nl1+options = l\nl1+r3 = f9\nl1+l3 = f8\nl1+l2 = ctrl+c\n";

enum { B_CROSS = 0x4000u, B_CIRCLE = 0x2000u, B_SQUARE = 0x8000u, B_TRIANGLE = 0x1000u, B_L1 = 0x400u,
       B_R1 = 0x800u, B_L2 = 0x100u, B_R2 = 0x200u, B_L3 = 0x2u, B_R3 = 0x4u, B_UP = 0x10u };

static PwGameInput map_input;
static PwWinePadMap map;
static PwWineInput ev[PW_WINE_PAD_MAP_EVENTS];
static char trace[1024];

/* One step at t ms, centred sticks unless given; the events as "+X -X" (keys
 * by their character or hex, mouse buttons as M0..M2). */
static const char *step_at(uint32_t buttons, uint64_t t_ms, uint8_t lx, uint8_t ly, int connected)
{
    size_t n = pw_wine_pad_map_step(&map_input, &map, buttons, lx, ly, 0x80, 0x80, connected, t_ms * 1000000ull,
                                    ev, PW_WINE_PAD_MAP_EVENTS), used = 0;
    trace[0] = 0;
    for (size_t i = 0; i < n; i++) {
        char name[8];
        if (ev[i].type == PW_WINE_INPUT_MOUSE_BUTTON) snprintf(name, sizeof(name), "M%u", ev[i].code);
        else if ((ev[i].code >= 'A' && ev[i].code <= 'Z') || (ev[i].code >= '0' && ev[i].code <= '9'))
            snprintf(name, sizeof(name), "%c", (char)ev[i].code);
        else snprintf(name, sizeof(name), "%02x", ev[i].code);
        used += (size_t)snprintf(trace + used, sizeof(trace) - used, "%s%c%s", used ? " " : "", ev[i].down ? '+' : '-', name);
    }
    return trace;
}
static const char *step(uint32_t buttons, uint64_t t_ms) { return step_at(buttons, t_ms, 0x80, 0x80, 1); }
#define STEP(b, t, want) do { const char *got_ = step((b), (t)); \
    if (strcmp(got_, (want))) { printf("t=%llu: got '%s' want '%s'\n", (unsigned long long)(t), got_, (want)); assert(0); } } while (0)
#define STICK(lx, ly, t, want) do { const char *got_ = step_at(0, (t), (lx), (ly), 1); \
    if (strcmp(got_, (want))) { printf("t=%llu: got '%s' want '%s'\n", (unsigned long long)(t), got_, (want)); assert(0); } } while (0)

static void map_reset(void)
{
    pw_game_input_init(&map_input);
    assert(pw_game_input_parse((const uint8_t *)BLOODLINES, sizeof(BLOODLINES) - 1, &map_input) == PW_OK);
    pw_wine_pad_map_init(&map);
}

static void test_pad_map(void)
{
    PwWineInput small[4];

    map_reset();
    /* The refusals. */
    assert(!pw_wine_pad_map_step(&map_input, &map, 0, 0x80, 0x80, 0x80, 0x80, 1, 0, small, 4));
    assert(!pw_wine_pad_map_step(NULL, &map, 0, 0x80, 0x80, 0x80, 0x80, 1, 0, ev, PW_WINE_PAD_MAP_EVENTS));

    /* Plain keys and mouse buttons, held while held. */
    STEP(B_CROSS, 0, "+E");
    STEP(B_CROSS | B_R2, 8, "+M0");
    STEP(0, 16, "-E -M0");

    /* Tap and hold at hold_ms (350) +-1: released at 349 taps r (down now, up
     * 20 ms later), held to 350 holds h until the release. */
    map_reset();
    STEP(B_SQUARE, 1000, "");
    STEP(B_SQUARE, 1349, "");
    STEP(0, 1349, "+R");
    STEP(0, 1368, "");                       /* the tap is held at least 20 ms */
    STEP(0, 1369, "-R");
    STEP(B_SQUARE, 2000, "");
    STEP(B_SQUARE, 2349, "");
    STEP(B_SQUARE, 2350, "+H");
    STEP(B_SQUARE, 2900, "");
    STEP(0, 2908, "-H");
    assert(map.stats.taps == 1 && map.stats.holds == 1);

    /* The layer: L1 sends nothing; L1+cross answers 1; a layer entry that is
     * unset falls through (R2 still attacks); L1 let go in the middle of a
     * held face button releases exactly what the press took. */
    map_reset();
    STEP(B_L1, 0, "");
    STEP(B_L1 | B_CROSS, 8, "+1");
    STEP(B_L1 | B_CROSS | B_R2, 16, "+M0");
    STEP(B_CROSS | B_R2, 24, "");            /* L1 up: 1 stays held by cross */
    STEP(B_R2, 32, "-1");
    STEP(0, 40, "-M0");
    STEP(B_CROSS, 48, "+E");                 /* the layer is gone: e again */
    STEP(B_CROSS | B_L1, 56, "");            /* L1 pressed while cross is held: nothing changes */
    STEP(B_L1, 64, "-E");
    STEP(0, 72, "");
    assert(map.stats.layer_keys == 1);
    /* An L1 tap/hold button: L1+square is the layer's 3, not a tap. */
    STEP(B_L1, 80, "");
    STEP(B_L1 | B_SQUARE, 88, "+3");
    STEP(B_L1, 96, "-3");
    STEP(0, 104, "");

    /* The latch: L3 holds ctrl until the next L3. */
    map_reset();
    STEP(B_L3, 0, "+11");
    STEP(0, 8, "");
    STEP(B_L3, 16, "-11");
    STEP(0, 24, "");
    assert(map.stats.toggles == 2);

    /* The chord's order, and a shared key: crouch latched, then L1+L2 sends
     * ctrl+c; ctrl stays held (the player stays crouched) after the chord. */
    map_reset();
    STEP(B_L1, 0, "");
    STEP(B_L1 | B_L2, 8, "+11 +C");
    STEP(B_L1, 16, "-C -11");
    STEP(0, 24, "");
    STEP(B_L3, 32, "+11");
    STEP(0, 40, "");
    STEP(B_L1, 48, "");
    STEP(B_L1 | B_L2, 56, "+C");             /* ctrl already held by the latch */
    STEP(B_L1, 64, "-C");                    /* ... and still held after */
    STEP(0, 72, "");
    assert(map.key_refs[0x11] == 1);
    STEP(B_L3, 80, "-11");
    STEP(0, 88, "");
    assert(map.stats.chords == 2);

    /* Walk and run: past the dead zone (31 counts) with less than 80% (101
     * counts) walks (shift with w, shift first); full push runs (shift let
     * go); hysteresis 5 counts either side; centred lets go. */
    map_reset();
    STICK(0x80, 128 - 60, 0, "+10 +W");
    STICK(0x80, 128 - 100, 8, "");           /* 100 <= 101 + 5: still walking */
    STICK(0x80, 128 - 107, 16, "-10");       /* past run + 5: runs */
    STICK(0x80, 128 - 100, 24, "");          /* 100 >= 101 - 5: still running */
    STICK(0x80, 128 - 95, 32, "+10");        /* inside run - 5: walks again */
    STICK(0x80, 0x80, 1032, "-W -10");
    assert(map.stats.stick_keys == 1 && map.stats.walk_ns == 1008000000ull);
    /* Walk shift held while a chord takes and lets go of shift: the walk keeps it. */
    pw_game_input_init(&map_input);
    {
        static const char text[] = "[input]\nkeyboard_stick = left_stick\nstick_walk = shift\nstick_run = 80\n"
                                   "cross = shift+e\n";
        assert(pw_game_input_parse((const uint8_t *)text, sizeof(text) - 1, &map_input) == PW_OK);
    }
    pw_wine_pad_map_init(&map);
    STICK(0x80, 128 - 60, 0, "+10 +W");
    assert(!strcmp(step_at(B_CROSS, 8, 0x80, 128 - 60, 1), "+E"));
    assert(!strcmp(step_at(0, 16, 0x80, 128 - 60, 1), "-E"));
    assert(map.key_refs[0x10] == 1);

    /* Disconnect lets go of everything and clears the latch. */
    map_reset();
    STEP(B_L3, 0, "+11");
    STEP(B_CROSS | B_R2, 8, "+E +M0");
    STEP(B_CROSS | B_R2 | B_SQUARE, 16, "");
    assert(!strcmp(step_at(B_CROSS | B_R2 | B_SQUARE, 24, 0x80, 0, 1), "+W"));
    assert(!strcmp(step_at(0, 32, 0x80, 0x80, 0), "-E -M0 -11 -W"));
    assert(map.latched[8] == 0 && map.key_refs['E'] == 0 && map.mouse_refs[0] == 0);
    STEP(0, 40, "");                         /* square's press was forgotten: no tap */
    assert(map.stats.stuck == 0);
}

/* stuck stays 0 over a random walk of buttons, sticks and disconnects, and
 * nothing is left held at the end. */
static void test_pad_map_fuzz(void)
{
    uint32_t seed = 12345u, buttons = 0;
    static const uint32_t pool[] = { B_CROSS, B_CIRCLE, B_SQUARE, B_TRIANGLE, B_L1, B_R1, B_L2, B_R2, B_L3, B_R3,
                                     B_UP, 0x40u, 0x80u, 0x20u, 0x8u };
    uint64_t t = 0;
    uint8_t shadow[256], shadow_mouse[3] = { 0, 0, 0 };

    memset(shadow, 0, sizeof(shadow));
    map_reset();
    for (int i = 0; i < 10000; i++) {
        uint8_t lx = 0x80, ly = 0x80;
        seed = seed * 1103515245u + 12345u;
        buttons ^= pool[(seed >> 16) % (sizeof(pool) / sizeof(pool[0]))];
        seed = seed * 1103515245u + 12345u;
        if ((seed >> 16) % 3 == 0) { lx = (uint8_t)(seed >> 8); ly = (uint8_t)(seed >> 20); }
        t += 4 + (seed >> 24) % 400;
        size_t n = pw_wine_pad_map_step(&map_input, &map, buttons, lx, ly, 0x80, 0x80, (seed >> 12) % 200 != 0,
                                        t * 1000000ull, ev, PW_WINE_PAD_MAP_EVENTS);
        /* What the game sees: never a second down without an up, never an up
         * without a down, and down exactly while the mapper counts a holder. */
        for (size_t e = 0; e < n; e++) {
            uint8_t *seen = ev[e].type == PW_WINE_INPUT_MOUSE_BUTTON ? &shadow_mouse[ev[e].code] : &shadow[ev[e].code];
            assert(*seen != ev[e].down);
            *seen = (uint8_t)ev[e].down;
        }
        for (size_t k = 0; k < 256; k++) assert(map.key_refs[k] < 64 && (shadow[k] != 0) == (map.key_refs[k] != 0));
        for (size_t m = 0; m < 3; m++) assert((shadow_mouse[m] != 0) == (map.mouse_refs[m] != 0));
    }
    assert(map.stats.stuck == 0);
    step_at(0, t + 1000, 0x80, 0x80, 0);
    for (size_t k = 0; k < 256; k++) assert(map.key_refs[k] == 0);
    for (size_t m = 0; m < 3; m++) assert(map.mouse_refs[m] == 0);
    assert(map.stats.stuck == 0);
}

int main(void)
{
    test_frames();
    test_threads();
    test_inputs();
    test_pointer();
    test_pointer_fraction();
    test_keyboard_stick();
    test_pad_map();
    test_pad_map_fuzz();
    test_pad();
    test_script_pad();
    printf("wine display passed: frame box copy, newest frame, refusals, concurrent put/take, "
           "profile bindings to keys and mouse buttons, stick pointer, a stick driving the direction keys, "
           "the pad mapper (layer, tap/hold, latch, chord, walk/run, shared keys, disconnect, 10k-step fuzz), resized frames, "
           "DualSense as XInput, scripted controller added to it\n");
    return 0;
}
