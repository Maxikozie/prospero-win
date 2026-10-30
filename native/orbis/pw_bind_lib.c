/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The exporter of milestone d1's two-PRX test: two functions and one data
 * object, which pw_bind_use/pw_bind_early (loader imports), pw_bind_thunk
 * (generated thunks) and pw_bind_data (a data import) take from it. The
 * counter counts pw_bind_add calls from any importer. */
#include <unistd.h>

int pw_bind_counter = 1000;
static volatile int pid;

int pw_bind_add(int a, int b)
{
    pw_bind_counter++;
    return a + b;
}

/* The getpid call gives the module a libkernel import, so that the link has
 * a .got.plt (create-fself requires the section). */
const char *pw_bind_name(void)
{
    pid = (int)getpid();
    return "pw_bind_lib";
}

int pw_bind_calls(void) { return pw_bind_counter - 1000; }
