/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_DUP_ORBIS_H
#define PW_WINE_DUP_ORBIS_H
/* A new descriptor for fd's open file, made by passing fd to this process
 * with SCM_RIGHTS (the PS4 refuses dup() and F_DUPFD); -1 with errno. */
int pw_orbis_dup_by_passing(int fd);
#endif
