/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The PS4 title's eboot: a stub that loads the title from
 * /app0/sce_module/ps4wine.prx and never runs again. The title releases this
 * eboot's segments before Wine starts (src/pw_eboot_release.h), so that a
 * Windows program fixed at 0x400000 can be mapped where the eboot was.
 *
 * The loader runs neither a PRX's constructors nor its module_start: the stub
 * finds the PRXDESC1 descriptor, calls module_start (which runs .init_array
 * once), then pw_title_main with the arguments it was given.
 *
 * The stub NEEDs every Sony library the title PRX NEEDs
 * (tools/build_native_orbis.sh checks it): a safeguard, as a PRX loaded after
 * the release ran with a library the eboot did not NEED (ps4-wine i1).
 *
 * From: plastikman/orbis-win src/reloc-wine-app/stub.c @ 2610706
 * (LGPL-2.1-or-later): load the PRX, read its segments, find the
 * descriptor's entry, call it with argc/argv, idle on any failure.
 */
#include "pw_wine_prx.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PW_TITLE_PRX "/app0/sce_module/ps4wine.prx"

extern uint32_t sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags,
                                         const void *option, int *result);
extern int sceKernelGetModuleInfo(int32_t handle, void *info);
extern int sceKernelDebugOutText(int channel, const char *text);
extern int sceKernelUsleep(uint32_t microseconds);

/* A PRX's thread-locals are relaxed to local-exec offsets into the main
 * executable's TLS block (wine/orbis/pw_emutls_orbis.c), so this eboot's
 * PT_TLS is where ps4wine.prx's and every Wine PRX's musl locale lives. It
 * stays .tbss only (no image in the segments that get released) and at least
 * as large as the largest PRX's: build_native_orbis.sh checks both. */
__thread uint64_t pw_stub_tls_room[8];

static void say(const char *text)
{
    sceKernelDebugOutText(0, text);
}

static void say_hex(const char *head, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char line[96];
    size_t n = strlen(head);

    if (n > sizeof(line) - 12) n = sizeof(line) - 12;
    memcpy(line, head, n);
    line[n++] = '0';
    line[n++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) line[n++] = digits[(value >> shift) & 0xf];
    line[n++] = '\n';
    line[n] = 0;
    say(line);
}

static void __attribute__((noreturn)) idle(void)
{
    for (;;) sceKernelUsleep(1000000);
}

int main(int argc, char **argv)
{
    uint8_t info[PW_PRX_MODULE_INFO_BYTES];
    uint64_t size_word = sizeof(info);
    PwPrxSegment segments[PW_PRX_MAX_SEGMENTS];
    const PwPrxDescriptor *descriptor = NULL;
    int (*module_start)(size_t, const void *);
    void (*title_main)(int, char **);
    uint32_t count = 0;
    int result = 0, rc;
    uint32_t handle;

    /* Keep the room linked: the SDK's link.x drops an unreferenced .tbss. */
    (void)*(volatile uint64_t *)&pw_stub_tls_room[0];
    say("PS4WINE stub: loading " PW_TITLE_PRX "\n");
    handle = sceKernelLoadStartModule(PW_TITLE_PRX, 0, NULL, 0, NULL, &result);
    if (handle & 0x80000000u) {
        say_hex("PS4WINE stub: the title PRX did not load: ", handle);
        idle();
    }
    memset(info, 0, sizeof(info));
    memcpy(info, &size_word, sizeof(size_word));
    rc = sceKernelGetModuleInfo((int32_t)handle, info);
    memcpy(info, &size_word, sizeof(size_word));
    if (rc || pw_prx_parse_module_info(info, NULL, segments, &count) != PW_PRX_OK ||
        pw_prx_find_descriptor(segments, count, &descriptor) != PW_PRX_OK) {
        say_hex("PS4WINE stub: no descriptor in the title PRX; module info ", (uint32_t)rc);
        idle();
    }
    module_start = (int (*)(size_t, const void *))(uintptr_t)pw_prx_lookup(descriptor, "module_start");
    title_main = (void (*)(int, char **))(uintptr_t)pw_prx_lookup(descriptor, "pw_title_main");
    if (!module_start || !title_main) {
        say("PS4WINE stub: module_start or pw_title_main missing\n");
        idle();
    }
    if ((rc = module_start(0, NULL))) {
        say_hex("PS4WINE stub: module_start returned ", (uint32_t)rc);
        idle();
    }
    title_main(argc, argv);
    /* Not reached: pw_title_main never returns, and this code may be gone by now. */
    idle();
}
