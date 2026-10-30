/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The environment of a Wine PRX on the PS4.
 *
 * Every OpenOrbis module links its own static musl, with its own environ:
 * a setenv in the title is invisible inside ntdll.prx, and ntdll's inside
 * wineserver.prx (measured: Wine left with "cannot get path to ntdll.so"
 * although the title had set WINE_PS5_NTDLL_DIR). So each PRX exports
 * pw_wine_setenv, the title sets Wine's variables through ntdll's copy,
 * and the loader shim (wine/ps5/pw_wine_dl.c) replays this image's
 * environment into every module it loads. */
#include "pw_wine_env_orbis.h"
#include <stdlib.h>
#include <string.h>

extern char **environ;

int pw_wine_setenv(const char *name, const char *value)
{
    if (!name || !*name) return -1;
    if (!value) return unsetenv(name);
    return setenv(name, value, 1);
}

void pw_wine_env_replicate(int (*set)(const char *name, const char *value))
{
    char name[256];

    if (!set || !environ) return;
    for (char **entry = environ; *entry; entry++) {
        const char *eq = strchr(*entry, '=');
        size_t n;
        if (!eq) continue;
        n = (size_t)(eq - *entry);
        if (!n || n >= sizeof(name)) continue;
        memcpy(name, *entry, n);
        name[n] = 0;
        (void)set(name, eq + 1);
    }
}
