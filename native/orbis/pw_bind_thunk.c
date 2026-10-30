/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* pw_bind_use's calls through thunks from tools/gen_prx_imports.py instead
 * of loader imports (milestone d1's fallback): the title fills the slots
 * from pw_bind_lib's PRXDESC1 table with pw_prx_bind_imports, then calls
 * pw_bind_thunk_run. */
#include <string.h>

extern int pw_bind_add(int a, int b);
extern const char *pw_bind_name(void);

int pw_bind_thunk_run(void)
{
    const char *name = pw_bind_name();
    return pw_bind_add(40, 2) == 42 && name && !strcmp(name, "pw_bind_lib");
}
