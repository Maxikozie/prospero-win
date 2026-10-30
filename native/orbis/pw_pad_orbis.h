/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The DualShock 4 through libScePad on the PS4: the signed-in user who
 * started the title, one pad, its state. The layout below is OpenOrbis's
 * OrbisPadData (120 bytes, as the PS5's DualSense state: connected at 76,
 * timestamp at 80, count at 104); milestone d1 measures how many bytes
 * scePadReadState writes before anything relies on it.
 */
#ifndef PW_PAD_ORBIS_H
#define PW_PAD_ORBIS_H

#include <stdint.h>

#define PW_PAD_ORBIS_STATE_BYTES 120
#define PW_PAD_ORBIS_CROSS 0x4000u
#define PW_PAD_ORBIS_CIRCLE 0x2000u

typedef struct PwPadOrbisState {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2;
    uint8_t rest[PW_PAD_ORBIS_STATE_BYTES - 10];
} PwPadOrbisState;
_Static_assert(sizeof(PwPadOrbisState) == PW_PAD_ORBIS_STATE_BYTES, "OrbisPadData is 120 bytes");
#define PW_PAD_ORBIS_CONNECTED_AT 76

typedef struct PwPadOrbis {
    int32_t user;
    int32_t handle;
    int init_status;        /* sceUserServiceInitialize's return */
    int status;             /* the first failing call's return, 0 once open */
    const char *failed;
} PwPadOrbis;

int pw_pad_orbis_open(PwPadOrbis *pad);
/* scePadReadState into state; 0 on success. */
int pw_pad_orbis_read(const PwPadOrbis *pad, PwPadOrbisState *state);
/* How many bytes of a 512-byte buffer scePadReadState wrote (the last byte
 * that differs from the fill), for the layout check; -1 on failure. */
int pw_pad_orbis_measure(const PwPadOrbis *pad);

#endif
