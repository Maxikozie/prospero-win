/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The environment of a Wine PRX on the PS4: see pw_wine_env_orbis.c. */
#ifndef PW_WINE_ENV_ORBIS_H
#define PW_WINE_ENV_ORBIS_H

/* Exported by every Wine PRX (descriptor table): setenv in that image. A
 * NULL value unsets. */
int pw_wine_setenv(const char *name, const char *value);

/* Calls set(name, value) for every variable of this image's environment. */
void pw_wine_env_replicate(int (*set)(const char *name, const char *value));

#endif /* PW_WINE_ENV_ORBIS_H */
