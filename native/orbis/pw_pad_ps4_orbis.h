/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The DualShock 4 behind the pad core of native/pw_pad_ps5.c: PwPadPs5Ops for
 * the PS4. The core does the batching, the connect/intercept handling, the
 * touchpad's halves and the edges; these ops reach the DS4.
 *
 * The pad is the title's one pad (pw_pad_orbis_open), opened before the title
 * released its eboot: scePadInit fails after the release (ps4-wine i1). So the
 * user and open ops hand over that pad, close leaves it open, and the user
 * service is never terminated from here.
 *
 * A DS4 sample (OpenOrbis's OrbisPadData) and a DualSense sample are both
 * 120 bytes with the same layout up to and including the touch block (bytes
 * 0-75); the DS4's connected is one byte at 76 where the DualSense has an
 * int32_t, followed by three padding bytes whose content is not known. The
 * read copies the whole sample, touch block included, and makes connected
 * that one byte.
 */
#ifndef PW_PAD_PS4_ORBIS_H
#define PW_PAD_PS4_ORBIS_H

#include "pw_pad_ps5.h"

enum { PW_PAD_PS4_SAMPLE_BYTES = 120, PW_PAD_PS4_CONNECTED_AT = 76 };

/* One DS4 sample (PW_PAD_PS4_SAMPLE_BYTES at raw) as the core's sample. */
void pw_pad_ps4_convert(const uint8_t *raw, PwPadPs5Data *out);
/* The ops (the pad must have been opened with pw_pad_orbis_open). */
int pw_pad_ps4_orbis_ops(PwPadPs5Ops *ops);

#endif
