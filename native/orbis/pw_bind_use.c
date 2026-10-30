/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* An importer of milestone d1's two-PRX test, built twice: pw_bind_use
 * (loaded after pw_bind_lib) and pw_bind_early (loaded before it, to see
 * whether the loader finds a NEEDED application module by itself). Its two
 * calls are imports of pw_bind_lib's library, which the console loader binds
 * or does not. Returns 1 when both answer as pw_bind_lib does. */
#include <string.h>

extern int pw_bind_add(int a, int b);
extern const char *pw_bind_name(void);

int pw_bind_use_run(void)
{
    const char *name = pw_bind_name();
    return pw_bind_add(40, 2) == 42 && name && !strcmp(name, "pw_bind_lib");
}
