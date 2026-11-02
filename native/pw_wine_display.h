/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_DISPLAY_H
#define PW_WINE_DISPLAY_H
/*
 * The title's side of Wine's PS5 user driver (wine/ps5/pw_wine_sink.h).
 *
 * win32u hands each frame to the present sink on one of Wine's threads. The
 * sink only copies it into a PwWineFrameBox; the title's own thread takes the
 * newest frame and shows it, so the display backend never runs on a Wine
 * thread and a slow flip never stalls Wine. The same thread reads the pad and
 * turns it into Wine input with the game profile's bindings: buttons send
 * keys or mouse buttons, and a stick can move the pointer. In xinput mode it
 * also becomes the game's XInput controller (the sink's gamepad slot).
 */
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "../src/pw_present.h"
#include "../src/pw_game_profile.h"
#include "../src/pw_script_input.h"
#include "../wine/ps5/pw_wine_sink.h"
#include "pw_pad_ps5.h"

typedef struct PwWineFrameBox {
    pthread_mutex_t lock;
    uint8_t *pixels;              /* the caller's storage; rows packed, width * 4 bytes */
    size_t capacity;
    uint32_t width, height;
    uint64_t sequence;            /* frames put */
    uint64_t rejected;            /* too large, or invalid */
} PwWineFrameBox;

/* A box keeping frames of at most bytes (width * height * 4) in storage,
 * which the caller owns: a title's libc heap is too small for frames, so it
 * lends static memory. 0, or -1. */
int pw_wine_frame_box_init(PwWineFrameBox *box, uint8_t *storage, size_t bytes);
void pw_wine_frame_box_destroy(PwWineFrameBox *box);
/* Keep a copy of a BGRA frame, top-down, stride in bytes. 0, or -1 when it
 * is invalid or larger than the box. */
int pw_wine_frame_box_put(PwWineFrameBox *box, const void *bgra, uint32_t width,
                          uint32_t height, uint32_t stride);
/* When a frame newer than *seen was put, copy it into out (at most
 * capacity bytes), describe it in view, update *seen and return 1; 0 when
 * there is nothing new, -1 when out is too small. */
int pw_wine_frame_box_take(PwWineFrameBox *box, uint64_t *seen, uint8_t *out,
                           size_t capacity, PwPresentView *view);

/* Wine input for one pad batch's edges under a profile's bindings: a
 * release for each bound button in released, then a press for each in
 * pressed, as a key or a mouse button. Returns how many were written (at
 * most max). */
size_t pw_wine_game_inputs(const PwGameInput *input, uint32_t pressed, uint32_t released,
                           PwWineInput *out, size_t max);

/* The DualSense as an XInput gamepad: its held buttons (Cross A, Circle B,
 * Square X, Triangle Y, L1/R1 shoulders, L3/R3 thumbs, Options Start,
 * Create Back, a touchpad click Back on its left half and Guide on its
 * right, by where it began: touch_left), the analog triggers, and the sticks
 * scaled to -32768..32767 with y up. 1 and the state in out while the pad
 * is connected, 1 and a neutral state while the system has it
 * (intercepted); 0 and a neutral state otherwise. The sink numbers the
 * packets. */
int pw_wine_game_pad(const PwPadPs5 *pad, PwWinePad *out);

/* A script build's scripted controller (src/pw_script_input.h) added to
 * the real one in pad, which pw_wine_game_pad filled (connected says
 * whether it is): the scripted buttons join the held ones, and each stick
 * the script holds off centre takes the real stick's place. With scripted
 * set (the macro drives a controller), controller 0 stays connected
 * without a DualSense. Returns whether it is connected; a neutral pad
 * when not. */
int pw_wine_script_pad(PwWinePad *pad, int connected, const PwScriptPad *script, int scripted);

/* A stick's pointer motion not yet sent, in 1/65536 pixels. Wine keeps
 * the pointer's position: the title sends only motion, as a mouse does
 * (patch 0670), so a game can move or clip the pointer itself and motion
 * against an edge still reaches it. */
typedef struct PwWinePointer {
    int64_t x, y;
} PwWinePointer;

enum { PW_WINE_POINTER_DEADZONE = 20 };   /* of 128, stick noise at rest */

/* Move by a stick position (0..255 each axis, 0x80 centred) held for
 * elapsed_us, at up to speed pixels per second at full tilt, on a squared
 * curve past the dead zone. 1 and a MOUSE_MOVE of the whole pixels moved
 * in out when there are any (the fraction is kept for the next step),
 * else 0. */
int pw_wine_pointer_step(PwWinePointer *pointer, uint8_t stick_x, uint8_t stick_y,
                         uint32_t speed, uint32_t elapsed_us, PwWineInput *out);

/* Digital key emulation for one analog stick (upstream prospero-win #736,
 * 58b6e0c). Direction bits are retained across frames so key-down is sent
 * only on a threshold crossing, and every held direction is released when
 * the controller disconnects.
 *
 * For titles that read the keyboard for movement and cannot be configured
 * to use a stick axis: one stick drives the four direction keys. */
typedef struct PwWineKeyboardStick { uint8_t held; } PwWineKeyboardStick;
size_t pw_wine_keyboard_stick_step(const PwGameInput *input, uint8_t stick_x, uint8_t stick_y,
                                   int connected, PwWineKeyboardStick *state,
                                   PwWineInput *out, size_t max);

/* The pad mapper: a profile's whole keyboard-mode input for one pad, step
 * by step (the PS4 title's play loop). Each step takes the held buttons, the
 * sticks and the time and returns the KEY and MOUSE_BUTTON events to post:
 *
 * - A button's action is chosen when it is pressed, from the layer button's
 *   state at that moment (its <layer>+ binding, else its own), and its
 *   release lets go of exactly what that press took, so pressing or
 *   releasing the layer button never leaves a key held.
 * - A button with a .hold binding sends nothing on its press: released
 *   before hold_ms it taps its own binding (down now, up after
 *   PW_WINE_PAD_TAP_NS, so a game sampling once a frame sees it); held
 *   hold_ms it holds the hold binding until released.
 * - toggle:<key> latches the key on one press and lets it go on the next; a
 *   chord holds its modifier around its key.
 * - keyboard_stick's direction keys, and stick_walk held with them while the
 *   stick is pushed less than stick_run percent (5 counts of hysteresis).
 * - Keys are counted: a key goes down when its first holder takes it and up
 *   when its last lets go, so a latch, the walk key and a chord can share
 *   ctrl or shift without one releasing the other.
 * - A step with connected 0 lets go of everything and clears the latches.
 *
 * Stuck counts keys found down that no holder accounts for (the invariant
 * is 0). out must hold PW_WINE_PAD_MAP_EVENTS events; 0 is returned for a
 * smaller one. */
enum { PW_WINE_PAD_MAP_EVENTS = 192, PW_WINE_PAD_MAP_TAPS = 16 };
#define PW_WINE_PAD_TAP_NS 20000000ull
typedef struct PwWinePadMapStats {
    uint64_t stick_keys;    /* direction keys pressed by the stick */
    uint64_t walk_ns;       /* time the walk key was held by the stick */
    uint64_t layer_keys;    /* presses that took their <layer>+ binding */
    uint64_t taps, holds, toggles, chords;
    uint64_t stuck;         /* keys found down with no holder (each step that finds one) */
    uint64_t events;        /* events returned */
} PwWinePadMapStats;
typedef struct PwWinePadMap {
    uint32_t buttons;                       /* held at the last step */
    int layer_down;
    struct {
        uint8_t phase;                      /* PwWinePadPhase (pw_wine_display.c) */
        PwGameBinding took;                 /* what the press holds */
        uint64_t since_ns;
    } press[PW_GAME_BUTTON_COUNT];
    uint8_t latched[PW_GAME_BUTTON_COUNT];
    PwGameBinding latch[PW_GAME_BUTTON_COUNT];
    struct { PwGameBinding binding; uint64_t up_ns; } taps[PW_WINE_PAD_MAP_TAPS];
    uint32_t tap_count;
    PwWineKeyboardStick stick;
    uint8_t walk;
    uint64_t last_ns;
    uint8_t key_refs[256];
    uint8_t mouse_refs[3];
    PwWinePadMapStats stats;
} PwWinePadMap;
void pw_wine_pad_map_init(PwWinePadMap *map);
size_t pw_wine_pad_map_step(const PwGameInput *input, PwWinePadMap *map, uint32_t buttons, uint8_t left_x,
                            uint8_t left_y, uint8_t right_x, uint8_t right_y, int connected, uint64_t now_ns,
                            PwWineInput *out, size_t max);
#endif
