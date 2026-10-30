/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The smallest 64-bit console program: what the PS4 title runs first
 * (the PS4 port's milestone c: "Wine core boots a 64-bit console EXE"). Its one
 * line reaches the title through Wine's standard output handle, fd 1, and
 * its exit status through the exit hook. Built with x86_64-w64-mingw32-gcc;
 * tests/test_wine_hello64.py runs it under the host Wine. */
#include <stdio.h>

int main(void)
{
    puts("Hello from PE64");
    fflush(stdout);
    return 0;
}
