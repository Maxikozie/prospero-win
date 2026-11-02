/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_display.h"
#include <string.h>

int pw_wine_frame_box_init(PwWineFrameBox *box, uint8_t *storage, size_t bytes)
{
    if (!box || !storage || !bytes) return -1;
    memset(box, 0, sizeof(*box));
    if (pthread_mutex_init(&box->lock, NULL)) return -1;
    box->pixels = storage;
    box->capacity = bytes;
    return 0;
}

void pw_wine_frame_box_destroy(PwWineFrameBox *box)
{
    if (!box || !box->pixels) return;
    pthread_mutex_destroy(&box->lock);
    box->pixels = NULL;
}

int pw_wine_frame_box_put(PwWineFrameBox *box, const void *bgra, uint32_t width,
                          uint32_t height, uint32_t stride)
{
    size_t row, bytes;

    if (!box || !box->pixels) return -1;
    row = (size_t)width * 4u;
    bytes = row * height;
    pthread_mutex_lock(&box->lock);
    if (!bgra || !width || !height || stride < row || bytes / height != row ||
        bytes > box->capacity) {
        box->rejected++;
        pthread_mutex_unlock(&box->lock);
        return -1;
    }
    for (uint32_t y = 0; y < height; y++)
        memcpy(box->pixels + y * row, (const uint8_t *)bgra + (size_t)y * stride, row);
    box->width = width;
    box->height = height;
    box->sequence++;
    pthread_mutex_unlock(&box->lock);
    return 0;
}

int pw_wine_frame_box_take(PwWineFrameBox *box, uint64_t *seen, uint8_t *out,
                           size_t capacity, PwPresentView *view)
{
    size_t bytes;
    int status = 0;

    if (!box || !box->pixels || !seen || !out || !view) return -1;
    pthread_mutex_lock(&box->lock);
    if (box->sequence != *seen) {
        bytes = (size_t)box->width * 4u * box->height;
        if (bytes > capacity) status = -1;
        else {
            memcpy(out, box->pixels, bytes);
            view->pixels = out;
            view->width = box->width;
            view->height = box->height;
            view->stride = box->width * 4u;
            view->bytes = (uint32_t)bytes;
            *seen = box->sequence;
            status = 1;
        }
    }
    pthread_mutex_unlock(&box->lock);
    return status;
}

static size_t add_edges(const PwGameInput *input, uint32_t edges, uint32_t down,
                        PwWineInput *out, size_t used, size_t max)
{
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT && used < max; i++) {
        const PwGameBinding *b = &input->bindings[i];
        if (!(edges & b->mask) || (b->kind != PW_GAME_BIND_KEY && b->kind != PW_GAME_BIND_MOUSE))
            continue;
        out[used].type = b->kind == PW_GAME_BIND_KEY ? PW_WINE_INPUT_KEY : PW_WINE_INPUT_MOUSE_BUTTON;
        out[used].code = b->code;
        out[used].x = out[used].y = 0;
        out[used].down = down;
        used++;
    }
    return used;
}

size_t pw_wine_game_inputs(const PwGameInput *input, uint32_t pressed, uint32_t released,
                           PwWineInput *out, size_t max)
{
    size_t used;

    if (!input || !out) return 0;
    used = add_edges(input, released, 0, out, 0, max);
    return add_edges(input, pressed, 1, out, used, max);
}

/* From upstream prospero-win #736 (58b6e0c), the same project's
 * keyboard_stick. */
enum { KEY_STICK_UP = 1u << 0, KEY_STICK_DOWN = 1u << 1,
       KEY_STICK_LEFT = 1u << 2, KEY_STICK_RIGHT = 1u << 3 };

static int stick_direction(uint8_t value, int negative, int was_held, unsigned deadzone)
{
    /* Five raw stick counts of hysteresis keep a noisy axis from tapping the
     * key repeatedly at the edge of its dead zone. */
    int threshold = (int)(127u * deadzone / 100u);
    int delta = (int)value - 128;
    if (negative) return delta < -(threshold - (was_held ? 5 : 0));
    return delta > threshold - (was_held ? 5 : 0);
}

size_t pw_wine_keyboard_stick_step(const PwGameInput *input, uint8_t stick_x, uint8_t stick_y,
                                   int connected, PwWineKeyboardStick *state,
                                   PwWineInput *out, size_t max)
{
    static const uint8_t bits[4] = { KEY_STICK_UP, KEY_STICK_DOWN, KEY_STICK_LEFT, KEY_STICK_RIGHT };
    uint8_t wanted = 0, previous;
    size_t used = 0;

    if (!input || !state || !out) return 0;
    previous = state->held;
    if (connected && input->keyboard_stick != PW_GAME_STICK_NONE) {
        if (stick_direction(stick_y, 1, !!(previous & KEY_STICK_UP), input->stick_deadzone))
            wanted |= KEY_STICK_UP;
        if (stick_direction(stick_y, 0, !!(previous & KEY_STICK_DOWN), input->stick_deadzone))
            wanted |= KEY_STICK_DOWN;
        if (stick_direction(stick_x, 1, !!(previous & KEY_STICK_LEFT), input->stick_deadzone))
            wanted |= KEY_STICK_LEFT;
        if (stick_direction(stick_x, 0, !!(previous & KEY_STICK_RIGHT), input->stick_deadzone))
            wanted |= KEY_STICK_RIGHT;
    }
    /* Release first so a direct direction reversal cannot momentarily hold
     * both keys. The caller reserves room for all four transitions. */
    for (size_t i = 0; i < 4 && used < max; i++) {
        if ((previous & bits[i]) && !(wanted & bits[i]))
            out[used++] = (PwWineInput){ PW_WINE_INPUT_KEY, input->stick_keys[i], 0, 0, 0 };
    }
    for (size_t i = 0; i < 4 && used < max; i++) {
        if (!(previous & bits[i]) && (wanted & bits[i]))
            out[used++] = (PwWineInput){ PW_WINE_INPUT_KEY, input->stick_keys[i], 0, 0, 1 };
    }
    state->held = wanted;
    return used;
}

/* ---- the pad mapper (pw_wine_display.h) -------------------------------------- */

typedef enum PwWinePadPhase {
    PAD_IDLE = 0,       /* nothing held for this press */
    PAD_TOOK,           /* the press holds took */
    PAD_PENDING,        /* a tap/hold button waiting: tap on release, hold at hold_ms */
    PAD_HOLDING,        /* took is the hold binding */
    PAD_LAYER,          /* the layer button */
} PwWinePadPhase;

typedef struct MapOut { PwWineInput *out; size_t used, max; } MapOut;

static void emit(MapOut *o, uint32_t type, uint32_t code, uint32_t down)
{
    if (o->used < o->max) o->out[o->used++] = (PwWineInput){ type, code, 0, 0, down };
}

static void key_take(PwWinePadMap *map, MapOut *o, uint16_t vk)
{
    if (!vk) return;
    if (map->key_refs[vk & 0xff]++ == 0) emit(o, PW_WINE_INPUT_KEY, vk & 0xff, 1);
}

static void key_drop(PwWinePadMap *map, MapOut *o, uint16_t vk)
{
    if (!vk || !map->key_refs[vk & 0xff]) return;
    if (--map->key_refs[vk & 0xff] == 0) emit(o, PW_WINE_INPUT_KEY, vk & 0xff, 0);
}

/* A binding taken: a chord's modifier first, then its key. */
static void take(PwWinePadMap *map, MapOut *o, const PwGameBinding *b)
{
    if (b->kind == PW_GAME_BIND_MOUSE && b->code < 3) {
        if (map->mouse_refs[b->code]++ == 0) emit(o, PW_WINE_INPUT_MOUSE_BUTTON, b->code, 1);
    } else if (b->kind == PW_GAME_BIND_KEY) {
        key_take(map, o, b->mod);
        key_take(map, o, b->code);
    }
}

/* Let go in the reverse order: the key, then the chord's modifier. */
static void drop(PwWinePadMap *map, MapOut *o, const PwGameBinding *b)
{
    if (b->kind == PW_GAME_BIND_MOUSE && b->code < 3) {
        if (map->mouse_refs[b->code] && --map->mouse_refs[b->code] == 0)
            emit(o, PW_WINE_INPUT_MOUSE_BUTTON, b->code, 0);
    } else if (b->kind == PW_GAME_BIND_KEY) {
        key_drop(map, o, b->code);
        key_drop(map, o, b->mod);
    }
}

static int binding_live(const PwGameBinding *b)
{
    return b->kind == PW_GAME_BIND_KEY || b->kind == PW_GAME_BIND_MOUSE;
}

/* A tap: the binding now, let go after PW_WINE_PAD_TAP_NS. */
static void tap(PwWinePadMap *map, MapOut *o, const PwGameBinding *b, uint64_t now)
{
    if (!binding_live(b)) return;
    take(map, o, b);
    map->stats.taps++;
    if (map->tap_count < PW_WINE_PAD_MAP_TAPS) {
        map->taps[map->tap_count].binding = *b;
        map->taps[map->tap_count].up_ns = now + PW_WINE_PAD_TAP_NS;
        map->tap_count++;
    } else {
        drop(map, o, b);    /* no room to wait: down and up in one step */
    }
}

/* A press's own binding: a latch flips, anything else is held until release. */
static void press_binding(PwWinePadMap *map, MapOut *o, size_t i, const PwGameBinding *b)
{
    map->press[i].phase = PAD_IDLE;
    if (!binding_live(b)) return;
    if (b->toggle) {
        if (map->latched[i]) {
            drop(map, o, &map->latch[i]);
            map->latched[i] = 0;
        } else {
            map->latch[i] = *b;
            take(map, o, b);
            map->latched[i] = 1;
        }
        map->stats.toggles++;
        return;
    }
    take(map, o, b);
    if (b->mod) map->stats.chords++;
    map->press[i].took = *b;
    map->press[i].phase = PAD_TOOK;
}

static void release_all(PwWinePadMap *map, MapOut *o)
{
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++) {
        if (map->press[i].phase == PAD_TOOK || map->press[i].phase == PAD_HOLDING) drop(map, o, &map->press[i].took);
        map->press[i].phase = PAD_IDLE;
        if (map->latched[i]) drop(map, o, &map->latch[i]);
        map->latched[i] = 0;
    }
    for (uint32_t t = 0; t < map->tap_count; t++) drop(map, o, &map->taps[t].binding);
    map->tap_count = 0;
    map->buttons = 0;
    map->layer_down = 0;
}

/* The holders each key should have, against the counts: a key down with no
 * holder is stuck (and let go, so one fault does not stay). */
static void check_stuck(const PwGameInput *input, PwWinePadMap *map, MapOut *o)
{
    uint8_t want[256], want_mouse[3] = { 0, 0, 0 };
    int found = 0;

    memset(want, 0, sizeof(want));
#define WANT(b) do { const PwGameBinding *w_ = (b); \
        if (w_->kind == PW_GAME_BIND_MOUSE && w_->code < 3) want_mouse[w_->code]++; \
        else if (w_->kind == PW_GAME_BIND_KEY) { if (w_->mod) want[w_->mod & 0xff]++; want[w_->code & 0xff]++; } } while (0)
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++) {
        if (map->press[i].phase == PAD_TOOK || map->press[i].phase == PAD_HOLDING) WANT(&map->press[i].took);
        if (map->latched[i]) WANT(&map->latch[i]);
    }
    for (uint32_t t = 0; t < map->tap_count; t++) WANT(&map->taps[t].binding);
#undef WANT
    for (size_t d = 0; d < 4; d++)
        if (map->stick.held & (1u << d)) want[input->stick_keys[d] & 0xff]++;
    if (map->walk) want[input->stick_walk & 0xff]++;
    for (size_t k = 1; k < 256; k++)
        if (map->key_refs[k] > want[k]) {
            if (!want[k]) { emit(o, PW_WINE_INPUT_KEY, (uint32_t)k, 0); found = 1; }
            map->key_refs[k] = want[k];
        }
    for (size_t m = 0; m < 3; m++)
        if (map->mouse_refs[m] > want_mouse[m]) {
            if (!want_mouse[m]) { emit(o, PW_WINE_INPUT_MOUSE_BUTTON, (uint32_t)m, 0); found = 1; }
            map->mouse_refs[m] = want_mouse[m];
        }
    if (found) map->stats.stuck++;
}

void pw_wine_pad_map_init(PwWinePadMap *map)
{
    if (map) memset(map, 0, sizeof(*map));
}

size_t pw_wine_pad_map_step(const PwGameInput *input, PwWinePadMap *map, uint32_t buttons, uint8_t left_x,
                            uint8_t left_y, uint8_t right_x, uint8_t right_y, int connected, uint64_t now_ns,
                            PwWineInput *out, size_t max)
{
    MapOut o = { out, 0, max };
    PwWineInput moved[4];
    size_t count;
    uint32_t pressed, released;
    uint8_t sx, sy;

    if (!input || !map || !out || max < PW_WINE_PAD_MAP_EVENTS) return 0;
    if (map->walk && map->last_ns && now_ns > map->last_ns) map->stats.walk_ns += now_ns - map->last_ns;
    map->last_ns = now_ns;

    /* Taps whose time has come let go first. */
    for (uint32_t t = 0; t < map->tap_count;) {
        if (map->taps[t].up_ns <= now_ns) {
            drop(map, &o, &map->taps[t].binding);
            map->taps[t] = map->taps[--map->tap_count];
        } else {
            t++;
        }
    }
    if (!connected) {
        release_all(map, &o);
        count = pw_wine_keyboard_stick_step(input, 0x80, 0x80, 0, &map->stick, moved, 4);
        for (size_t k = 0; k < count; k++) key_drop(map, &o, (uint16_t)moved[k].code);
        if (map->walk) key_drop(map, &o, input->stick_walk);
        map->walk = 0;
        check_stuck(input, map, &o);
        map->stats.events += o.used;
        return o.used;
    }

    pressed = buttons & ~map->buttons;
    released = map->buttons & ~buttons;
    /* Releases before presses. */
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++) {
        uint32_t mask = pw_game_button_mask(i);
        if (!(released & mask)) continue;
        switch (map->press[i].phase) {
        case PAD_LAYER: map->layer_down = 0; break;
        case PAD_TOOK:
        case PAD_HOLDING: drop(map, &o, &map->press[i].took); break;
        case PAD_PENDING: tap(map, &o, &input->bindings[i], now_ns); break;
        default: break;
        }
        map->press[i].phase = PAD_IDLE;
    }
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++) {
        uint32_t mask = pw_game_button_mask(i);
        if (!(pressed & mask)) continue;
        map->press[i].since_ns = now_ns;
        if (input->layer >= 0 && (size_t)input->layer == i) {
            map->press[i].phase = PAD_LAYER;
            map->layer_down = 1;
        } else if (map->layer_down && input->layer_bindings[i].kind != PW_GAME_BIND_UNSET) {
            map->stats.layer_keys++;
            press_binding(map, &o, i, &input->layer_bindings[i]);
        } else if (binding_live(&input->hold_bindings[i])) {
            map->press[i].phase = PAD_PENDING;
        } else {
            press_binding(map, &o, i, &input->bindings[i]);
        }
    }
    /* Holds that have come of age. */
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++) {
        if (map->press[i].phase != PAD_PENDING || now_ns - map->press[i].since_ns < (uint64_t)input->hold_ms * 1000000u)
            continue;
        map->press[i].took = input->hold_bindings[i];
        take(map, &o, &map->press[i].took);
        map->press[i].phase = PAD_HOLDING;
        map->stats.holds++;
    }
    map->buttons = buttons;

    /* The stick: the walk key first when it starts, after the directions when it stops. */
    sx = input->keyboard_stick == PW_GAME_STICK_RIGHT ? right_x : left_x;
    sy = input->keyboard_stick == PW_GAME_STICK_RIGHT ? right_y : left_y;
    count = pw_wine_keyboard_stick_step(input, sx, sy, 1, &map->stick, moved, 4);
    {
        int walk = 0;
        if (input->keyboard_stick != PW_GAME_STICK_NONE && input->stick_walk && input->stick_run && map->stick.held) {
            int dx = (int)sx - 128, dy = (int)sy - 128, m, run = (int)(127u * input->stick_run / 100u);
            dx = dx < 0 ? -dx : dx;
            dy = dy < 0 ? -dy : dy;
            m = dx > dy ? dx : dy;
            walk = map->walk ? m <= run + 5 : m < run - 5;
        }
        if (walk && !map->walk) key_take(map, &o, input->stick_walk);
        for (size_t k = 0; k < count; k++) {
            if (moved[k].down) {
                key_take(map, &o, (uint16_t)moved[k].code);
                map->stats.stick_keys++;
            } else {
                key_drop(map, &o, (uint16_t)moved[k].code);
            }
        }
        if (!walk && map->walk) key_drop(map, &o, input->stick_walk);
        map->walk = (uint8_t)walk;
    }
    check_stuck(input, map, &o);
    map->stats.events += o.used;
    return o.used;
}

static const struct { uint32_t dualsense; uint16_t xinput; } pad_buttons[] = {
    { 0x4000u, 0x1000u }, { 0x2000u, 0x2000u }, { 0x8000u, 0x4000u }, { 0x1000u, 0x8000u },
    { 0x400u, 0x0100u }, { 0x800u, 0x0200u }, { 0x2u, 0x0040u }, { 0x4u, 0x0080u },
    { 0x10u, 0x0001u }, { 0x40u, 0x0002u }, { 0x80u, 0x0004u }, { 0x20u, 0x0008u },
    { 0x8u, 0x0010u }, { 0x1u, 0x0020u },
};

/* One stick axis (0..255, 0x80 centred) as XInput's -32768..32767, centre
 * 0; up is 0 on the DualSense and positive in XInput. */
static int16_t pad_axis(uint8_t raw, int up)
{
    int32_t d = up ? 0x80 - (int32_t)raw : (int32_t)raw - 0x80;   /* -127..128 or -128..127 */
    int32_t reach = d < 0 ? (up ? 127 : 128) : (up ? 128 : 127);
    return (int16_t)(d * (d < 0 ? 32768 : 32767) / reach);
}

int pw_wine_game_pad(const PwPadPs5 *pad, PwWinePad *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    /* While the system has the pad the controller stays connected, idle: a
     * game told it was unplugged pauses or drops it (DuckStation did, and
     * did not take it back). */
    if (!pad || (!pad->core.connected && !pad->intercepted)) return 0;
    out->connected = 1;
    if (pad->intercepted) return 1;
    for (size_t i = 0; i < sizeof(pad_buttons) / sizeof(pad_buttons[0]); i++)
        if (pad->core.previous_buttons & pad_buttons[i].dualsense) out->buttons |= pad_buttons[i].xinput;
    /* The touchpad is two buttons: Back on its left half, so Select needs
     * no Create (the system's capture button), Guide on its right. */
    if (pad->core.previous_buttons & PW_PAD_PS5_TOUCHPAD) out->buttons |= pad->touch_left ? 0x0020u : 0x0400u;
    out->left_trigger = pad->l2;
    out->right_trigger = pad->r2;
    out->thumb_lx = pad_axis(pad->left_stick.x, 0);
    out->thumb_ly = pad_axis(pad->left_stick.y, 1);
    out->thumb_rx = pad_axis(pad->right_stick.x, 0);
    out->thumb_ry = pad_axis(pad->right_stick.y, 1);
    return 1;
}

int pw_wine_script_pad(PwWinePad *pad, int connected, const PwScriptPad *script, int scripted)
{
    if (!pad) return 0;
    if (!connected) memset(pad, 0, sizeof(*pad));
    if (!script || !scripted) return connected ? 1 : 0;
    pad->connected = 1;
    pad->buttons |= script->buttons;
    if (script->stick[0][0] || script->stick[0][1]) {
        pad->thumb_lx = script->stick[0][0];
        pad->thumb_ly = script->stick[0][1];
    }
    if (script->stick[1][0] || script->stick[1][1]) {
        pad->thumb_rx = script->stick[1][0];
        pad->thumb_ry = script->stick[1][1];
    }
    return 1;
}

/* Signed 1/65536-pixel travel of one axis. */
static int64_t travel(uint8_t stick, uint32_t speed, uint32_t elapsed_us)
{
    int32_t axis = (int32_t)stick - 0x80, magnitude = axis < 0 ? -axis : axis;
    int64_t t, v;

    if (magnitude <= PW_WINE_POINTER_DEADZONE) return 0;
    t = (int64_t)(magnitude - PW_WINE_POINTER_DEADZONE) * 65536 / (127 - PW_WINE_POINTER_DEADZONE);
    if (t > 65536) t = 65536;
    v = t * t >> 16;                       /* 0..65536: a squared curve */
    v = v * speed * elapsed_us / 1000000;   /* < 2^63 for speed <= 20000 */
    return axis < 0 ? -v : v;
}

int pw_wine_pointer_step(PwWinePointer *pointer, uint8_t stick_x, uint8_t stick_y,
                         uint32_t speed, uint32_t elapsed_us, PwWineInput *out)
{
    int64_t dx, dy;

    if (!pointer || !out) return 0;
    pointer->x += travel(stick_x, speed, elapsed_us);
    pointer->y += travel(stick_y, speed, elapsed_us);
    /* whole pixels, towards zero, so both directions keep their fraction */
    dx = pointer->x / 65536;
    dy = pointer->y / 65536;
    if (!dx && !dy) return 0;
    pointer->x -= dx * 65536;
    pointer->y -= dy * 65536;
    *out = (PwWineInput){ PW_WINE_INPUT_MOUSE_MOVE, 0, (int32_t)dx, (int32_t)dy, 0 };
    return 1;
}
