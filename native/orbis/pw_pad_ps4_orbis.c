/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * PwPadPs5Ops for the DualShock 4 (pw_pad_ps4_orbis.h).
 *
 * From: plastikman/orbis-win src/native/pw_pad_ps4.c @ 2610706
 * (LGPL-2.1-or-later): the PS4's filling of PwPadPs5Ops (one-line forwards,
 * {lgMotor, smMotor} for scePadSetVibration, a read into OrbisPadData copied
 * into the core's sample). Changed here: the user and open ops hand over the
 * title's pad opened before the eboot's release, close and terminate do
 * nothing, and the read keeps the touch block (orbis-win zeroed it, so a
 * click always read as the right half).
 */
#include "pw_pad_ps4_orbis.h"
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(PwPadPs5Data) == PW_PAD_PS4_SAMPLE_BYTES, "a DS4 sample is a DualSense sample's size");
_Static_assert(offsetof(PwPadPs5Data, connected) == PW_PAD_PS4_CONNECTED_AT, "connected at 76 on both");

void pw_pad_ps4_convert(const uint8_t *raw, PwPadPs5Data *out)
{
    memcpy(out, raw, PW_PAD_PS4_SAMPLE_BYTES);
    out->connected = raw[PW_PAD_PS4_CONNECTED_AT] ? 1 : 0;
}

#ifndef PW_PAD_PS4_ORBIS_HOST_TEST
#include "pw_pad_orbis.h"

extern int scePadRead(int32_t handle, void *data, int32_t count);
extern int scePadSetVibration(int32_t handle, const void *parameter);

static PwPadOrbis title_pad;

/* Not 0: the core then does not own (and never terminates) the user service. */
static int user_initialize(const void *parameters)
{
    (void)parameters;
    return 1;
}

static int foreground_user(int32_t *user)
{
    if (pw_pad_orbis_open(&title_pad)) return title_pad.status;
    *user = title_pad.user;
    return 0;
}

static int user_terminate(void) { return 0; }
static int pad_init(void) { return title_pad.status; }

static int pad_open(int32_t user, int32_t type, int32_t index, const void *parameters)
{
    (void)user; (void)type; (void)index; (void)parameters;
    return title_pad.status ? title_pad.status : title_pad.handle;
}

static int pad_close(int32_t handle)
{
    (void)handle;
    return 0;
}

static int pad_read(int32_t handle, PwPadPs5Data *samples, int32_t count)
{
    uint8_t raw[PW_PAD_PS5_BATCH][PW_PAD_PS4_SAMPLE_BYTES];
    int got;

    if (count <= 0) return 0;
    if (count > PW_PAD_PS5_BATCH) count = PW_PAD_PS5_BATCH;
    got = scePadRead(handle, raw, count);
    if (got <= 0) return got;
    if (got > count) got = count;
    for (int i = 0; i < got; i++) pw_pad_ps4_convert(raw[i], &samples[i]);
    return got;
}

static int pad_set_vibration(int32_t handle, const uint8_t motors[2])
{
    /* OrbisPadVibeParam: {lgMotor, smMotor}, the order the core passes them. */
    const uint8_t parameter[2] = { motors[0], motors[1] };
    return scePadSetVibration(handle, parameter);
}

int pw_pad_ps4_orbis_ops(PwPadPs5Ops *ops)
{
    if (!ops) return PW_ERR_PRECONDITION;
    memset(ops, 0, sizeof(*ops));
    ops->user_initialize = user_initialize;
    ops->foreground_user = foreground_user;
    ops->user_terminate = user_terminate;
    ops->pad_init = pad_init;
    ops->pad_open = pad_open;
    ops->pad_read = pad_read;
    ops->pad_close = pad_close;
    ops->pad_set_vibration = pad_set_vibration;
    return PW_OK;
}
#endif
