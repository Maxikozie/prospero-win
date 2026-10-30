/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* How a Wine PRX leaves on the PS4.
 *
 * Wine ends a process through exit() (patches 0130 and 0896 route
 * NtTerminateProcess and a failed main executable there, so the exit
 * handlers run); the server's fatal_error and the shims' assertions end
 * in exit(1) or abort(). On the PS4 a title that calls exit() is reported
 * as a crash (CE-34878-0), and the PS5's restart through LoadExec is the
 * title's business, not the module's. So ntdll.prx and wineserver.prx are
 * linked with --wrap=exit,_exit,abort: the wraps hand the status to a hook
 * the title registered, which does not return (it reports the run and
 * idles, or restarts the title). Without a hook, the real calls run. */
#ifndef PW_WINE_EXIT_ORBIS_H
#define PW_WINE_EXIT_ORBIS_H

enum {
    PW_WINE_EXIT_STATUS_ABORT = -1,   /* abort(): status has no meaning */
};

/* kind: "exit", "_exit" or "abort". Must not return. */
typedef void (*PwWineExitHook)(void *context, int status, const char *kind);

/* Exported from ntdll.prx and wineserver.prx (each has its own copy of the
 * shim, so the title registers with both). A NULL hook restores the real
 * exit. */
void pw_wine_set_exit_hook(PwWineExitHook hook, void *context);

/* The hook this image has, for the loader shim to hand on to a module it
 * loads (the title registers with ntdll only; wineserver.prx is ntdll's). */
void pw_wine_get_exit_hook(PwWineExitHook *hook, void **context);

/* What the wraps do, for the host test: calls the hook, else the real one. */
void pw_wine_exit_dispatch(int status, const char *kind);

#endif /* PW_WINE_EXIT_ORBIS_H */
