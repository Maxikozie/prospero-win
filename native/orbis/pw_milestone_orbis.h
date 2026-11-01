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
 *   e1  RADV's libvulkan.prx driven by the title, the display handed over,
 *       hello.exe -> PS4WINE e1 vk ...
 *   e2  winevulkan's slots, the display, vk64.exe (Wine's Vulkan, then a
 *       Win32 swapchain) -> PS4WINE e2 vk64 ... and PS4WINE e3 vkclear ...
 *   e   the display, d3d11tri64.exe under DXVK 2.6.2 -> PS4WINE e d3d11 frames=N gpu_hang=0|1 ...
 *   f1  wowprospero.prx's slots, the address space below 4 GiB, the display,
 *       wow32.exe (32-bit, on the translator: checks, then gdi64's picture)
 *       -> PS4WINE f1 hello32 cpu=translator ... and PS4WINE f2 gdi32 ...
 *   f   wowprospero.prx's slots, the display, Space Cadet Pinball (C:\Pinball)
 *       on a 960x540 desktop, played with the DS4 until the touchpad is held
 *       -> PS4WINE f pinball cpu=translator frames=N fps=N faults_unresolved=N ...
 *   p1  plan f's stages; Pinball's attract mode with the pad unused: 20 s
 *       from the first present, then a 60 s segment measured per present
 *       (phase p, docs/ps4/HANDOFF-p.md in ps4-wine); hand play after it
 *       -> PS4WINE p1 bench pinball cpu=translator ... seg_s=60 inputs=0 presents=N guest_us_* ...
 *   p3  p1 on A8R8G8B8_SRGB scan-out buffers (vo720: 1280x720 on a 1280x720
 *       desktop, a row copy, VideoOut asked to scale; vo1080: 1920x1080, no
 *       swizzle, the 2x scale by rows), after a test pattern shown 5 s for
 *       the HDMI check
 *       -> PS4WINE p3 bench pinball ... present_path=vo720|vo1080 vo_register=ok vo_flip=ok copy_us_avg=N ...
 *   p4  p3's path (or env.txt's) with three buffers and an asynchronous flip
 *       -> PS4WINE p4 bench pinball present_mode=async buffers=3 shown_hz=N flip_errors=N ...
 *       In p1, p3 and p4, env.txt's PS4WINE_PRESENT_PATH=scale|vo720|vo1080 and
 *       PS4WINE_PRESENT_MODE=sync|async replace the plan's defaults.
 *   p5  p4's path with Wine's audio driver on the title's sceAudioOut port,
 *       then a scripted tail of keys that plays Pinball's sounds
 *       -> PS4WINE p5 bench pinball audio=on audio_open=ok ... grains_nonzero=N ...
 *          flex_mib_start=N flex_mib=N threads_created=N threads_raised=N stack_floor_mib=N ...
 *   p5w wowprospero.prx's slots, the display, wincb32.exe (tests/wine_wincb.c:
 *       windows and callbacks on the main thread and on new threads, then 8
 *       threads alive at once while the title samples the free flexible memory)
 *       -> PS4WINE p5w wincb bits=32 cpu=translator cases=23 cases_ok=N main_ok=N/N new_ok=N/N
 *          ... hold_flex_kib=N threads_created=N threads_raised=N stack_floor_mib=N ...
 *       env.txt's PS4WINE_EXE (any plan) names another program, e.g. C:\wincb64.exe.
 *   p6  the thread stacks: libkernel's own 16 MiB stack created, joined and
 *       created again (stack_probe, no Wine), then p5w's wincb32.exe, whose
 *       8 hold threads come twice (the second 8 after the first have ended)
 *       -> PS4WINE p6 stacks stack_policy=keep|small|floor hold_flex_kib=N hold_back_kib=N
 *          hold2_flex_kib=N threads_kept=N threads_small=N threads_keep_refused=N ...
 *          sp_lib_rc=0x0,0x0 sp_lib_alive1_kib=N sp_lib_kib1=N sp_lib_alive2_kib=N sp_lib_kib2=N ...
 *       env.txt's WINE_PS5_THREAD_STACK (every plan: read by wine64_main_orbis.c
 *       and handed to ntdll.prx's pthread layer before Wine starts) chooses what a
 *       thread whose caller set its stack (Wine's spawn_thread) gets: keep (the
 *       default), small (512 KiB of the layer's own) or floor (the main thread's
 *       size, as before). The e line carries the same thread fields and
 *       libvulkan.prx's own layer: vk_threads_created vk_stack_floor_kib.
 *
 * The plan comes from PS4WINE_MILESTONE in <root>/env.txt (default: the newest).
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
    /* NAME=VALUE from <root>/env.txt into out (NULL when absent): the p plans'
     * PS4WINE_PRESENT_PATH and PS4WINE_PRESENT_MODE. May be NULL. */
    const char *(*setting)(const char *name, char *out, size_t size);
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
/* ntdll's flush sink (pw_wine_set_flush_sink, patch 0490): what the user
 * driver's next present changed, for phase p's measurements. */
void pw_ms_flush(void *context, uint32_t dirty_px, uint32_t screen_px, uint32_t composed);
/* ntdll's audio sink (pw_wine_set_audio_sink): one 256-frame S16 stereo
 * grain to the title's sceAudioOut port, blocking (phase p5). */
int pw_ms_audio(void *context, const int16_t *frames);
/* Whether the plan plays audio and its port is open: set the sink only then. */
int pw_ms_audio_wanted(void);
/* ntdll's pw_wine_post_input, for the scripted input of d4. */
void pw_ms_set_input(int (*post)(const void *event));
/* ntdll's display release (pw_wine_set_display_release): closes the
 * title's VideoOut, on the main thread (pw_ms_service); 0 once closed. */
int pw_ms_release_display(void *context);
/* The title's main loop: performs a release asked for from another thread. */
void pw_ms_service(void);
/* The Wine stage's watchdog in seconds, 0 for the title's default. */
unsigned pw_ms_wine_watchdog_s(void);
/* Each line the Wine program writes to stdout. */
void pw_ms_stdout(const char *line);
/* The plan's defaults for Wine's environment (RADV's variables in the e
 * plans), added before <root>/env.txt is read. */
void pw_ms_environment(void (*add)(const char *name, const char *value));
/* "module+0xoffset" for an address, "?" outside every loaded module. */
const char *pw_orbis_module_of(uint64_t address, char *out, size_t size);

#endif
