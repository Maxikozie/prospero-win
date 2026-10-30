/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The PS4 port's milestone launches in the title: a plan of stages with a
 * progress file (pw_stage_orbis.h), the last stage being the Wine program,
 * and the plan's PS4WINE lines at the end.
 *
 *   d1  cross-PRX binding, the title's VideoOut, the DS4, hello.exe
 *       -> PS4WINE d1 bind=import|thunk|none ... hello=ok ...
 *   d2  win32u's and libfreetype's ntdll slots, the display, gdi64.exe with
 *       its frames on VideoOut -> PS4WINE d2 gdi ... and PS4WINE d3 fonts ...
 *   d4  the display, winemine and a scripted click on its board
 *       -> PS4WINE d4 winemine frames=N board_opened=0|1 refused=N fb=ok ...
 *   d   d4's click, then the DualShock 4 drives the pointer until OPTIONS
 *       -> PS4WINE d winemine frames=N inputs=N refused=N board_opened=0|1 fb=ok ...
 *
 * The plan comes from PS4WINE_MILESTONE in <root>/env.txt (default d).
 * Title stages run under a fault handler that records the fault and goes
 * on with the next stage; each has a time limit a watchdog thread enforces
 * (on overrun: an H record, the lines with what is known, idle). The Wine
 * stage has milestone c's own fault and watchdog paths; the title reports
 * its end through pw_ms_wine_end.
 */
#ifndef PW_MILESTONE_ORBIS_H
#define PW_MILESTONE_ORBIS_H

#include <stddef.h>
#include <stdint.h>

typedef struct PwMsConfig {
    const char *module_dir;              /* the runtime's x86_64-unix directory */
    const char *root;                    /* PW_ROOT */
    const char *build_id;
    void (*log)(const char *line);       /* diagnostics log (netlog) */
    void (*emit)(const char *line);      /* klog + netlog + log: records that must arrive */
    uint64_t (*now_ns)(void);
} PwMsConfig;

/* Picks the plan by name ("d1", "d2"); NULL or unknown keeps the default.
 * Returns the plan's name. */
const char *pw_ms_select(const char *name);
/* The Windows program of the plan's Wine stage. */
const char *pw_ms_exe(void);
/* Whether the plan reports milestone c's line too (d1: hello.exe). */
int pw_ms_c_line(void);
/* Reads the plan's progress file, installs the stage fault handler, starts the watchdog. */
void pw_ms_start(const PwMsConfig *config);
/* The title stages, each unless done or lost in an earlier launch. */
void pw_ms_title_stages(void);
int pw_ms_wine_should_run(void);
/* B: for the Wine stage, and the default handlers back for Wine's start. */
void pw_ms_wine_begin(void);
/* The Wine stage's end: its exit value as the c line has it, the program's
 * last stdout line, whether the server connected; then the plan's lines. */
void pw_ms_wine_end(const char *exit_text, const char *last_stdout, int server_connected);
/* The plan's lines (once per launch); then the progress file is renamed
 * when every stage has ended. */
void pw_ms_emit_lines(void);
/* The present sink for ntdll's pw_wine_set_present_sink: BGRA frames from
 * the user driver, shown on the title's VideoOut (d2). */
int pw_ms_present(void *context, const void *bgra, uint32_t width, uint32_t height, uint32_t stride);
/* ntdll's pw_wine_post_input, for the scripted input of d4. */
void pw_ms_set_input(int (*post)(const void *event));
/* "module+0xoffset" for an address, "?" outside every loaded module. */
const char *pw_orbis_module_of(uint64_t address, char *out, size_t size);

#endif
