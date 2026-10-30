/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * ntdll.prx's dlfcn under names no system library exports, for the PRXs
 * that import from ntdll (win32u, linked --wrap=dlopen,dlsym,...).
 *
 * libkernel exports dlopen, dlsym, dlclose and dlerror as well, and
 * create-fself gives an import to the first NEEDED library that exports
 * its name, libkernel always first: measured in the PS4 port's milestone
 * d1, win32u's dlsym slot held libkernel's dlsym although ld.lld had bound
 * it to ntdll. These forwarders reach wine/ps5/pw_wine_dl_libc.c's
 * definitions, which look names up in the loaded modules' descriptors.
 */
#include <dlfcn.h>

void *__wrap_dlopen(const char *path, int mode) { return dlopen(path, mode); }
void *__wrap_dlsym(void *__restrict handle, const char *__restrict name) { return dlsym(handle, name); }
int __wrap_dlclose(void *handle) { return dlclose(handle); }
char *__wrap_dlerror(void) { return dlerror(); }
int __wrap_dladdr(const void *address, Dl_info *info) { return dladdr(address, info); }
