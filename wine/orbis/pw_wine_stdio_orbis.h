/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Where a Wine PRX's standard output and error go on the PS4: see
 * pw_wine_stdio_orbis.c. */
#ifndef PW_WINE_STDIO_ORBIS_H
#define PW_WINE_STDIO_ORBIS_H

/* Forgets the descriptors read from the environment (the host test). */
void pw_wine_stdio_reset(void);

#endif /* PW_WINE_STDIO_ORBIS_H */
