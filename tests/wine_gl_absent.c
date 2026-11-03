/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Loads the modules a Direct3D game reaches opengl32 through (Wine's
 * builtin d3dx9_40 and d3dcompiler_47 import wined3d, which imports
 * opengl32) and prints one line:
 *   GLABSENT bits=32 opengl32=ok wined3d=ok d3dcompiler_47=ok d3dx9_40=ok
 * with err:<GetLastError()> for a module that does not load (1114 is
 * ERROR_DLL_INIT_FAILED: a DllMain returned FALSE). */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    static const char *const names[] = { "opengl32", "wined3d", "d3dcompiler_47", "d3dx9_40" };
    char line[256];
    int n = snprintf(line, sizeof(line), "GLABSENT bits=%d", (int)(sizeof(void *) * 8));

    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char file[64];
        HMODULE module;
        snprintf(file, sizeof(file), "%s.dll", names[i]);
        module = LoadLibraryA(file);
        if (module) n += snprintf(line + n, sizeof(line) - n, " %s=ok", names[i]);
        else n += snprintf(line + n, sizeof(line) - n, " %s=err:%lu", names[i], GetLastError());
    }
    printf("%s\n", line);
    fflush(stdout);
    return 0;
}
