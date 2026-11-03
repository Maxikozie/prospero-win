/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/orbis/pw_loaddll_orbis.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char base[24] = "-";
    const char *vampire = "0024:trace:loaddll:build_module Loaded L\"C:\\\\VTMB\\\\Vampire.exe\" at 00400000: native";
    const char *game = "WINE 0024:trace:loaddll:build_module Loaded L\"C:\\\\Psychonauts\\\\Psychonauts.exe\" at "
                       "00400000: native";
    const char *ntdll = "0024:trace:loaddll:build_ntdll_module Loaded L\"C:\\\\windows\\\\system32\\\\ntdll.dll\" at "
                        "00006FFFFFF30000: builtin";
    const char *spaces = "trace:loaddll:build_module Loaded L\"C:\\\\Program Files\\\\My Game\\\\My Game.exe\" at "
                         "10000000: native";

    assert(!strcmp(pw_loaddll_file_name("C:\\Psychonauts\\Psychonauts.exe"), "Psychonauts.exe"));
    assert(!strcmp(pw_loaddll_file_name("C:/g1/d3d9tri32.exe"), "d3d9tri32.exe"));
    assert(!strcmp(pw_loaddll_file_name("plain.exe"), "plain.exe"));

    assert(pw_loaddll_base(vampire, "Vampire.exe", base, sizeof(base)) && !strcmp(base, "0x00400000"));
    assert(pw_loaddll_base(game, "psychonauts.EXE", base, sizeof(base)) && !strcmp(base, "0x00400000"));
    assert(pw_loaddll_base(ntdll, "ntdll.dll", base, sizeof(base)) && !strcmp(base, "0x00006FFFFFF30000"));
    assert(pw_loaddll_base(spaces, "My Game.exe", base, sizeof(base)) && !strcmp(base, "0x10000000"));

    /* Not the module asked for, a longer or shorter name, not a load line: OUT untouched. */
    strcpy(base, "-");
    assert(!pw_loaddll_base(game, "Vampire.exe", base, sizeof(base)));
    assert(!pw_loaddll_base(game, "nauts.exe", base, sizeof(base)));
    assert(!pw_loaddll_base(game, "Psychonauts.exe2", base, sizeof(base)));
    assert(!pw_loaddll_base("err:module:import_dll Library binkw32.dll not found", "binkw32.dll", base, sizeof(base)));
    assert(!pw_loaddll_base("Loaded L\"C:\\\\x.dll\" at : native", "x.dll", base, sizeof(base)));
    assert(!pw_loaddll_base(game, "", base, sizeof(base)));
    assert(!strcmp(base, "-"));

    assert(pw_loaddll_is(ntdll, "NTDLL.DLL"));
    assert(pw_loaddll_is("Loaded L\"C:\\\\Psychonauts\\\\OpenAL32.dll\" at 10000000: native", "openal32.dll"));
    assert(!pw_loaddll_is("Loaded L\"C:\\\\Psychonauts\\\\OpenAL32.dll\" at 10000000: native", "openal.dll"));
    assert(!pw_loaddll_is("Loaded L\"C:\\\\Psychonauts\\\\OpenAL32.dll\"", "openal32.dll"));

    printf("test_pw_loaddll_orbis: a module's base from +loaddll by file name (any case, spaces), "
           "non-matches leave the base\n");
    return 0;
}
