/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A scripted title pad for host tests of Wine's xinput (patches 0470/0471):
 * preloaded (LD_PRELOAD) into the host Wine, it exports the two functions
 * xinput's Unix library looks up with dlsym(RTLD_DEFAULT), pw_wine_pad_at and
 * pw_wine_set_rumble_at, so xinput takes the title-pad path as on the
 * console, where ntdll.prx's sink exports them.
 *
 * Controller 0 is connected; 1..3 are not. The calls for controller 0 in
 * the served process walk a fixed script, FP_HOLD calls a step (xinput's
 * GetCapabilities and SetState read the pad too, and a real one holds a
 * state for many reads), then stay on the last step (neutral): every button
 * bit of 0xf3ff alone, each stick axis at -32768 and at 32767, each trigger
 * at 255. Every step is a new packet. So a program that polls a hundred
 * times has seen every button, every extreme and both triggers.
 *
 *   PW_SINK_PROCESS  only a process whose command line contains this serves
 *                    the pad (explorer and services load xinput too); others
 *                    read every controller disconnected
 *
 * stderr gets "fake_pad rumble index=I left=L right=R" for each rumble call
 * in the served process and "fake_pad summary reads=N steps=N rumble_calls=N
 * rumble_on=N" when it unloads. */
#define _GNU_SOURCE
#include "../wine/ps5/pw_wine_sink.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FP_EXPORT __attribute__((visibility("default")))

/* One step: buttons, triggers, then lx ly rx ry. */
typedef struct FpStep { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; } FpStep;

static const FpStep fp_script[] = {
    { 0x0001u, 0, 0, 0, 0, 0, 0 }, { 0x0002u, 0, 0, 0, 0, 0, 0 }, { 0x0004u, 0, 0, 0, 0, 0, 0 },
    { 0x0008u, 0, 0, 0, 0, 0, 0 }, { 0x0010u, 0, 0, 0, 0, 0, 0 }, { 0x0020u, 0, 0, 0, 0, 0, 0 },
    { 0x0040u, 0, 0, 0, 0, 0, 0 }, { 0x0080u, 0, 0, 0, 0, 0, 0 }, { 0x0100u, 0, 0, 0, 0, 0, 0 },
    { 0x0200u, 0, 0, 0, 0, 0, 0 }, { 0x1000u, 0, 0, 0, 0, 0, 0 }, { 0x2000u, 0, 0, 0, 0, 0, 0 },
    { 0x4000u, 0, 0, 0, 0, 0, 0 }, { 0x8000u, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, -32768, 0, 0, 0 }, { 0, 0, 0, 32767, 0, 0, 0 },
    { 0, 0, 0, 0, -32768, 0, 0 }, { 0, 0, 0, 0, 32767, 0, 0 },
    { 0, 0, 0, 0, 0, -32768, 0 }, { 0, 0, 0, 0, 0, 32767, 0 },
    { 0, 0, 0, 0, 0, 0, -32768 }, { 0, 0, 0, 0, 0, 0, 32767 },
    { 0, 255, 0, 0, 0, 0, 0 }, { 0, 0, 255, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 },
};
enum { FP_STEPS = sizeof(fp_script) / sizeof(fp_script[0]), FP_HOLD = 4 };

static pthread_mutex_t fp_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t fp_reads, fp_rumble_calls, fp_rumble_on;
static uint32_t fp_step, fp_held;

/* Whether this process is the one that serves the pad. */
static int fp_is_target(void)
{
    static int known = -1;
    if (known < 0) {
        const char *name = getenv("PW_SINK_PROCESS");
        char line[4096] = {0};
        FILE *f = fopen("/proc/self/cmdline", "rb");
        size_t n = f ? fread(line, 1, sizeof(line) - 1, f) : 0;
        if (f) fclose(f);
        for (size_t i = 0; i < n; i++) if (!line[i]) line[i] = ' ';
        known = !name || !*name || strstr(line, name) != NULL;
    }
    return known;
}

FP_EXPORT int pw_wine_pad_at(uint32_t index, PwWinePad *pad)
{
    const FpStep *step;
    if (!pad) return 0;
    memset(pad, 0, sizeof(*pad));
    if (index != 0 || !fp_is_target()) return 0;
    pthread_mutex_lock(&fp_lock);
    fp_reads++;
    step = &fp_script[fp_step];
    pad->connected = 1;
    pad->packet = fp_step + 1;
    pad->buttons = step->buttons;
    pad->left_trigger = step->lt;
    pad->right_trigger = step->rt;
    pad->thumb_lx = step->lx;
    pad->thumb_ly = step->ly;
    pad->thumb_rx = step->rx;
    pad->thumb_ry = step->ry;
    if (++fp_held == FP_HOLD && fp_step + 1 < FP_STEPS) fp_step++, fp_held = 0;
    pthread_mutex_unlock(&fp_lock);
    return 1;
}

FP_EXPORT void pw_wine_set_rumble_at(uint32_t index, uint32_t left, uint32_t right)
{
    if (!fp_is_target()) return;
    pthread_mutex_lock(&fp_lock);
    fp_rumble_calls++;
    if (left || right) fp_rumble_on++;
    pthread_mutex_unlock(&fp_lock);
    fprintf(stderr, "fake_pad rumble index=%u left=%u right=%u\n", index, left, right);
    fflush(stderr);
}

__attribute__((destructor)) static void fp_unloaded(void)
{
    if (!fp_reads && !fp_rumble_calls) return;
    fprintf(stderr, "fake_pad summary reads=%llu steps=%u rumble_calls=%llu rumble_on=%llu\n",
            (unsigned long long)fp_reads, fp_step + 1, (unsigned long long)fp_rumble_calls,
            (unsigned long long)fp_rumble_on);
    fflush(stderr);
}
