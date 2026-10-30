/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The DualShock 4 through libScePad (pw_pad_orbis.h). */
#include "pw_pad_orbis.h"

#include <string.h>

extern int32_t sceUserServiceInitialize(const void *parameters);
extern int32_t sceUserServiceGetInitialUser(int32_t *user);
extern int32_t scePadInit(void);
extern int32_t scePadOpen(int32_t user, int32_t type, int32_t index, const void *parameters);
extern int32_t scePadReadState(int32_t handle, void *state);

static int fail(PwPadOrbis *pad, const char *call, int status)
{
    pad->status = status ? status : -1;
    pad->failed = call;
    return pad->status;
}

int pw_pad_orbis_open(PwPadOrbis *pad)
{
    int32_t rc;

    memset(pad, 0, sizeof(*pad));
    pad->handle = -1;
    /* Its status is recorded, not fatal: a second initialisation in one
     * process reports an error, and whether the system did one first is
     * not measured. The initial user is what matters. */
    pad->init_status = sceUserServiceInitialize(NULL);
    if ((rc = sceUserServiceGetInitialUser(&pad->user))) return fail(pad, "sceUserServiceGetInitialUser", rc);
    if ((rc = scePadInit())) return fail(pad, "scePadInit", rc);
    if ((pad->handle = scePadOpen(pad->user, 0, 0, NULL)) < 0) return fail(pad, "scePadOpen", pad->handle);
    return 0;
}

int pw_pad_orbis_read(const PwPadOrbis *pad, PwPadOrbisState *state)
{
    return scePadReadState(pad->handle, state);
}

int pw_pad_orbis_measure(const PwPadOrbis *pad)
{
    uint8_t buffer[512];
    int last = -1;

    memset(buffer, 0xa5, sizeof(buffer));
    if (scePadReadState(pad->handle, buffer)) return -1;
    for (int i = 0; i < (int)sizeof(buffer); i++)
        if (buffer[i] != 0xa5) last = i;
    return last + 1;
}
