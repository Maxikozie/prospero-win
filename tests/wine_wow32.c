/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The PS4 port's milestone f1 + f2 program: Wine runs one program per
 * launch on the console (there is no process creation), so the 32-bit
 * checks of wine_hello32.c and the GDI picture of wine_gdi64.c, built for
 * i686, share one executable: the checks first (HELLO32, HELLO32S, "Hello
 * from PE32"), then the picture (GDI32). Exit 0 only if both parts
 * returned 0. Built with i686-w64-mingw32-gcc -O1 -fno-omit-frame-pointer
 * -lgdi32 -luser32; tests/test_wine_wow32.py runs it under the host Wine. */
#define main hello32_main
#include "wine_hello32.c"
#undef main
#define main gdi_main
#include "wine_gdi64.c"
#undef main

int main(void)
{
    int checks = hello32_main();
    int picture = gdi_main();

    return checks || picture ? 1 : 0;
}
