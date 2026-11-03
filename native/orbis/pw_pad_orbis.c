/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The DualShock 4 through libScePad (pw_pad_orbis.h). */
#include "pw_pad_orbis.h"

#include <pthread.h>
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

/* One open per process: scePadInit fails once the title has released the
 * eboot (ps4-wine i1: scePadOpen 0x80920005 after the release), so the title
 * opens the pad before the release and every later caller gets that pad. */
static pthread_mutex_t shared_lock = PTHREAD_MUTEX_INITIALIZER;
static PwPadOrbis shared;
static int shared_done;

static int open_once(PwPadOrbis *pad)
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

int pw_pad_orbis_open(PwPadOrbis *pad)
{
    pthread_mutex_lock(&shared_lock);
    if (!shared_done) {
        open_once(&shared);
        shared_done = 1;
    }
    *pad = shared;
    pthread_mutex_unlock(&shared_lock);
    return pad->status;
}

int pw_pad_orbis_opened(void)
{
    int done;
    pthread_mutex_lock(&shared_lock);
    done = shared_done;
    pthread_mutex_unlock(&shared_lock);
    return done;
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
