/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Milestone d1 in the PS4 title: cross-PRX binding, the title's VideoOut,
 * the DualShock 4 and hello.exe as separate stages with a progress file
 * (pw_stage_orbis.h), and one line at the end:
 *
 *   PS4WINE d1 bind=import|thunk|none lib=... import=... ... hello=... stages=N/9 ...
 *
 * Stages 1-8 are title code under a fault handler that records the fault
 * and goes on with the next stage; each has a time limit a watchdog thread
 * enforces (on overrun: an H record, the line with what is known, idle).
 * The hello stage is Wine itself, with the c milestone's own fault and
 * watchdog paths; the title reports its end through pw_d1_hello_end.
 */
#ifndef PW_D1_ORBIS_H
#define PW_D1_ORBIS_H

#include <stddef.h>
#include <stdint.h>

enum {
    PW_D1_BIND_EARLY,    /* an importer loaded before its exporter */
    PW_D1_BIND_LIB,      /* the exporter */
    PW_D1_BIND_IMPORT,   /* loader imports of functions, slots before and after a call */
    PW_D1_BIND_THUNK,    /* generated thunks filled from the exporter's PRXDESC1 table */
    PW_D1_BIND_DATA,     /* a loader import of a data object */
    PW_D1_W32U_BIND,     /* win32u.prx's ntdll slots */
    PW_D1_VIDEOOUT,      /* linear scan-out buffers, test pattern, readback */
    PW_D1_PAD,           /* DS4 open, state layout, cross */
    PW_D1_HELLO,         /* hello.exe (milestone c) */
    PW_D1_STAGES
};

typedef struct PwD1Config {
    const char *module_dir;              /* the runtime's x86_64-unix directory */
    const char *root;                    /* PW_ROOT */
    const char *build_id;
    void (*log)(const char *line);       /* diagnostics log (netlog) */
    void (*emit)(const char *line);      /* klog + netlog + log: records that must arrive */
    uint64_t (*now_ns)(void);
} PwD1Config;

/* Reads the progress file, installs the stage fault handler and starts the
 * watchdog thread. */
void pw_d1_start(const PwD1Config *config);
/* Stages 1-8, each unless done or lost in an earlier launch. */
void pw_d1_title_stages(void);
/* 1 if the hello stage is to run in this launch. */
int pw_d1_hello_should_run(void);
/* B:hello, and the default handlers back for Wine's start. */
void pw_d1_hello_begin(void);
/* The hello stage's result ("ok", or the c line's exit value), the d1
 * line and the cycle's end. Once; signal-safe as far as the file writes. */
void pw_d1_hello_end(const char *hello);
/* The d1 line (once per launch); then the progress file is renamed when
 * every stage has ended. */
void pw_d1_emit_line(void);
/* "module+0xoffset" for an address, "?" outside every loaded module. */
const char *pw_orbis_module_of(uint64_t address, char *out, size_t size);

#endif
