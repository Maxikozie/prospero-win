/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A module linked exactly like the Wine PRXs (tools/build_native_orbis.sh
 * --probe-mod): ld.lld --shared, the generated descriptor's global
 * module_start, no crtlib.o. The title loads it first and reports whether
 * its constructor ran (by the kernel, or once module_start is called) and
 * whether sceKernelDlsym resolves its export. */
#include <string.h>
#include <unistd.h>

static volatile int started;
static volatile int pid;
#ifdef PW_PROBE_TLS
/* pw_probe_tls: strerror pulls libc.a's locale member, whose thread-local
 * __musl_current_locale gives the image a PT_TLS segment as ntdll.prx has;
 * whether the loader accepts a module with one is what this build answers. */
static volatile const char *text;
#endif

/* The call into libkernel gives the module an import, so that the link has
 * a .got.plt as the Wine PRXs do (create-fself requires the section). */
__attribute__((constructor)) static void construct(void)
{
    pid = (int)getpid();
#ifdef PW_PROBE_TLS
    text = strerror(2);
#endif
    started = 0x5157;
}

int pw_probe_mod_started(void) { return started; }
