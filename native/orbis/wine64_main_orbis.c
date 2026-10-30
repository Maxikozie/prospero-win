/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The PS4 title (Orbis OS, OpenOrbis toolchain): loads ntdll.prx, starts
 * Wine on a 16 MiB-stack thread and runs one 64-bit console program, as
 * native/wine64_main.c does on the PS5 minus the launcher, display, pad,
 * audio and keyboard: the PS4 port's milestone c check is one line,
 *
 *   PS4WINE c hello.exe stdout="Hello from PE64" server=inprocess exit=0 ...
 *
 * emitted when Wine leaves (through the exit hook of wine/orbis) or when
 * the watchdog gives up, on the kernel log and the UDP log, after which the
 * title idles: on the PS4 a process that exits is reported as a crash.
 *
 * Before Wine, a milestone plan's stages run (pw_milestone_orbis.h; the
 * plan from PS4WINE_MILESTONE in <root>/env.txt, default d2): d1 cross-PRX
 * binding, the title's VideoOut, the DS4, then hello.exe; d2 win32u's and
 * FreeType's slots, the display, then gdi64.exe with its frames on
 * VideoOut. Each stage is recorded in a progress file, the Wine program is
 * the last stage, and the plan's "PS4WINE d1|d2|d3 ..." lines follow (d1
 * after milestone c's line).
 *
 * Arguments (from argv[0] on after a LoadExec restart, from argv[1] on at a
 * fresh launch): path=<absolute Windows path> (default C:\hello.exe).
 *
 * Paths: the runtime under <root>/runtime/win/wine (uploaded over FTP during
 * bring-up) or /app0/win/wine (shipped in the package); the prefix at
 * <root>/prefix; session logs under <root>/logs; NAME=VALUE lines in
 * <root>/env.txt join Wine's environment. <root> is PW_ROOT, a build-time
 * define (tools/build_native_orbis.sh --data-root), default /data/prospero-win.
 */
#include "pw_wine_start.h"
#include "pw_wine_launch.h"
#include "pw_diagnostics.h"
#include "pw_wine_prx.h"
#include "prospero_win.h"
#include "pw_wine_exit_orbis.h"
#include "pw_milestone_orbis.h"
#include "ps5log/ps5log.h"
#include "orbislog/orbislog.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PW_BUILD_ID
#define PW_BUILD_ID "unknown"
#endif
#ifndef PW_TITLE_ID
#define PW_TITLE_ID "PSWN00001"
#endif
#ifndef PW_ROOT
#define PW_ROOT "/data/prospero-win"
#endif
#define PW_RUNTIME "/win/wine/lib/wine/x86_64-unix"
#define PW_WATCHDOG_S 180

#define LOG(...) pw_diagnostics_log(__VA_ARGS__)

static void add_env(const char *name, const char *value);

/* libkernel, by prototype. */
extern uint32_t sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags,
                                         const void *option, int *result);
extern int sceKernelGetModuleInfo(int32_t handle, void *info);
extern int sceKernelDlsym(int32_t handle, const char *symbol, void **address);

/* ---- state the exit hook reports --------------------------------------- */

static PwWineStart start;
static int load_status = -1;
static volatile int server_connected;
static char last_stdout[200];
static volatile uint64_t last_stdout_ns;
static pthread_mutex_t stdout_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int reported;
static uint64_t start_ns, hello_ns;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t now_ns_hook(void) { return now_ns(); }

/* A record that must arrive: the kernel log, the UDP log and the diagnostics log. */
static void emit_line(const char *line)
{
    char datagram[1500];
    orbislog_klog(line);
    snprintf(datagram, sizeof(datagram), "%s\n", line);
    ps5log_raw(datagram, strlen(datagram));
    LOG("%s", line);
}

static void log_line(const char *line) { LOG("%s", line); }

/* Milestone c's line (in the d1 plan), every sink, once; then the plan's
 * Wine stage ends. */
static void report(const char *exit_text, const char *kind)
{
    char line[512], quoted[220];
    size_t n = 0;

    if (__atomic_exchange_n(&reported, 1, __ATOMIC_ACQ_REL)) return;
    pthread_mutex_lock(&stdout_lock);
    for (const char *p = last_stdout; *p && n < sizeof(quoted) - 2; p++)
        quoted[n++] = (*p == '"' || *p == '\\') ? '\'' : *p;
    quoted[n] = 0;
    pthread_mutex_unlock(&stdout_lock);
    snprintf(line, sizeof(line),
             "PS4WINE c hello.exe stdout=\"%s\" server=%s exit=%s build=%s load=%d stage=%d kind=%s "
             "session_s=%llu",
             quoted, server_connected ? "inprocess" : "none", exit_text, PW_BUILD_ID, load_status,
             start.stage, kind, (unsigned long long)((now_ns() - hello_ns) / 1000000000ull));
    if (pw_ms_c_line()) emit_line(line);
    else LOG("%s", line);
    pw_ms_wine_end(exit_text, quoted, server_connected);
}

/* NAME=VALUE in <root>/env.txt, before the plan starts (load_env_file reads
 * the whole file for Wine later). */
static const char *early_setting(const char *name, char *out, size_t size)
{
    char text[4096], *line, *next;
    size_t length = strlen(name);
    FILE *file = fopen(PW_ROOT "/env.txt", "rb");
    size_t got;

    if (!file) return NULL;
    got = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[got] = 0;
    for (line = text; line && *line; line = next) {
        if ((next = strchr(line, '\n'))) *next++ = 0;
        if (!strncmp(line, name, length) && line[length] == '=') {
            snprintf(out, size, "%s", line + length + 1);
            out[strcspn(out, "\r \t")] = 0;
            return out;
        }
    }
    return NULL;
}

/* ---- fd 1 and fd 2 -> the log ------------------------------------------- */

/* The in-process wineserver writes its errors with fprintf(stderr), and the
 * program's output reaches Wine's standard output handle, fd 1: each
 * becomes one end of a socket pair whose lines a thread forwards as
 * "WINESERVER ..." and "STDOUT ..." records. The last STDOUT line is what
 * the milestone line quotes. */
typedef struct PwFdForward { int reader, writer; const char *tag; } PwFdForward;

static void *forward_fd(void *arg)
{
    const PwFdForward *forward = arg;
    char buffer[1024];
    size_t used = 0;

    for (;;) {
        ssize_t got = read(forward->reader, buffer + used, sizeof(buffer) - 1 - used);
        if (got <= 0) break;
        used += (size_t)got;
        for (;;) {
            char *newline = memchr(buffer, '\n', used);
            if (!newline && used < sizeof(buffer) - 1) break;
            size_t line = newline ? (size_t)(newline - buffer) : used;
            buffer[line] = 0;
            if (line && buffer[line - 1] == '\r') buffer[line - 1] = 0;
            LOG("%s %s", forward->tag, buffer);
            if (forward->tag[0] == 'S' && buffer[0]) {
                pthread_mutex_lock(&stdout_lock);
                snprintf(last_stdout, sizeof(last_stdout), "%s", buffer);
                pthread_mutex_unlock(&stdout_lock);
                last_stdout_ns = now_ns();
            }
            line += newline ? 1u : 0u;
            memmove(buffer, buffer + line, used - line);
            used -= line;
        }
    }
    return NULL;
}

/* Close fd first: descriptors are allocated lowest first, so one end of the
 * new pair takes its number; otherwise dup2 moves the writer there. Both
 * consoles refuse that (measured: the pair comes back above 2 and dup2 onto
 * 1 or 2 fails), so the writer stays where it is and its number goes to
 * Wine in PW_ORBIS_STDOUT_FD / PW_ORBIS_STDERR_FD: ntdll's console setup
 * (patch 1010) makes the standard handles from those descriptors. */
static int capture_fd(int fd, PwFdForward *forward)
{
    int pair[2], rc, err;
    pthread_t thread;

    rc = close(fd);
    err = errno;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return -1;
    LOG("PW_WINE64 capture fd=%d close=%d errno=%d pair=%d,%d", fd, rc, rc ? err : 0, pair[0], pair[1]);
    if (pair[0] == fd || pair[1] == fd) {
        forward->reader = pair[0] == fd ? pair[1] : pair[0];
        forward->writer = fd;
    } else {
        forward->reader = pair[0];
        forward->writer = pair[1];
        if (dup2(pair[1], fd) == fd) forward->writer = fd;
        else LOG("PW_WINE64 capture fd=%d dup2 refused errno=%d; writer stays %d", fd, errno, pair[1]);
    }
    return pthread_create(&thread, NULL, forward_fd, forward) ? -4 : 0;
}

/* ---- Wine's debug output -------------------------------------------------- */

static void wine_output_sink(const char *str, size_t len)
{
    static char line[1024];
    static size_t used;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n' || used == sizeof(line) - 1) {
            line[used] = 0;
            LOG("WINE %s", line);
            if (strstr(line, "wine startup: server connected")) server_connected = 1;
            used = 0;
            if (str[i] != '\n') line[used++] = str[i];
        } else {
            line[used++] = str[i];
        }
    }
    pthread_mutex_unlock(&lock);
}

/* ---- the exit hook ------------------------------------------------------- */

/* Bytes the program wrote just before leaving may still sit in the fd 1
 * socket pair: wait for the forwarder to go quiet (at most 500 ms). */
static void drain_stdout(void)
{
    uint64_t t0 = now_ns();
    for (;;) {
        uint64_t now = now_ns(), last = last_stdout_ns;
        if (now - t0 > 500000000ull) break;
        if (last && now - last < 100000000ull) { usleep(20000); continue; }
        usleep(50000);
        if (now_ns() - t0 > 150000000ull && last_stdout_ns == last) break;
    }
}

static void exit_hook(void *context, int status, const char *kind)
{
    char text[32];
    (void)context;
    drain_stdout();
    if (status == PW_WINE_EXIT_STATUS_ABORT) snprintf(text, sizeof(text), "abort");
    else snprintf(text, sizeof(text), "%d", status);
    LOG("PW_WINE64 exit status=%s kind=%s", text, kind);
    report(text, kind);
    pw_diagnostics_close("wine-exit");
    orbislog_klog("PS4WINE idle after exit");
    for (;;) sleep(60);
}

/* ---- early fault report ---------------------------------------------------- */

/* Until Wine installs its own handlers, a fault inside ntdll would end the
 * process silently; report where it happened relative to ntdll's segments.
 * RIP sits at ucontext offset 224, RBP at 136 (mcontext at 64: FreeBSD's
 * mc_rbp at 72), measured on the PS4 in milestone a. Every Orbis object is
 * built with frame pointers, so the RBP chain is a backtrace. */
static void early_fault(int sig, siginfo_t *info, void *opaque)
{
    uint64_t rip = 0, offset = 0, rbp = 0;
    int segment = -1;

    memcpy(&rip, (const uint8_t *)opaque + 224, sizeof(rip));
    memcpy(&rbp, (const uint8_t *)opaque + 136, sizeof(rbp));
    for (uint32_t i = 0; i < start.segment_count; i++) {
        uint64_t base = (uint64_t)(uintptr_t)start.segments[i].address;
        if (rip >= base && rip < base + start.segments[i].size) {
            segment = (int)i;
            offset = rip - base;
            break;
        }
    }
    pw_diagnostics_emergency(sig, (uintptr_t)(info ? info->si_addr : NULL), rip, segment, offset);
    {
        char line[300], where[120];

        snprintf(line, sizeof(line), "PS4WINE fault signal=%d addr=0x%llx rip=0x%llx ntdll_segment=%d offset=0x%llx in=%s",
                 sig, (unsigned long long)(uintptr_t)(info ? info->si_addr : NULL), (unsigned long long)rip,
                 segment, (unsigned long long)offset, pw_orbis_module_of(rip, where, sizeof(where)));
        orbislog_klog(line);
        LOG("%s", line);
        /* The frame chain: [rbp] is the caller's rbp, [rbp+8] the return address. */
        for (int depth = 0; depth < 24 && rbp && !(rbp & 7); depth++) {
            uint64_t next = 0, ret = 0;
            const uint8_t *frame = (const uint8_t *)(uintptr_t)rbp;
            if (rbp < 0x10000 || rbp > 0x7fffffffffffull) break;
            memcpy(&next, frame, sizeof(next));
            memcpy(&ret, frame + 8, sizeof(ret));
            if (!ret) break;
            LOG("PW_WINE64 frame %d ret=0x%llx %s", depth, (unsigned long long)ret, pw_orbis_module_of(ret, where, sizeof(where)));
            if (next <= rbp) break;
            rbp = next;
        }
    }
    report("fault", "signal");
    for (;;) sleep(60);
}

static void install_early_fault_report(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = early_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
}

/* ---- bindings for the start sequence ------------------------------------- */

static int32_t load_start(const char *path, size_t argc, const void *argv, uint32_t flags,
                          const void *option, int *result)
{
    return (int32_t)sceKernelLoadStartModule(path, argc, argv, flags, option, result);
}
static int module_info(int32_t handle, void *info) { return sceKernelGetModuleInfo(handle, info); }
/* Each PS4 module has its own environ (static musl), so Wine's variables are
 * set in ntdll.prx through its pw_wine_setenv export, and in the title. */
static int (*ntdll_setenv)(const char *, const char *);
static int set_env(const char *name, const char *value)
{
    int rc = setenv(name, value, 1);
    if (ntdll_setenv && ntdll_setenv(name, value) != 0) rc = -1;
    return rc;
}

struct thread_start { void (*entry)(void *); void *arg; };

static void *thread_trampoline(void *opaque)
{
    struct thread_start s = *(struct thread_start *)opaque;
    free(opaque);
    LOG("PW_WINE64 thread entered, calling __wine_main");
    s.entry(s.arg);
    LOG("PW_WINE64 __wine_main returned");
    return NULL;
}

static int start_thread(void (*entry)(void *), void *arg, size_t stack_bytes)
{
    pthread_attr_t attr;
    pthread_t thread;
    struct thread_start *s = malloc(sizeof(*s));
    int status;

    if (!s) return -1;
    s->entry = entry;
    s->arg = arg;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_bytes);
    status = pthread_create(&thread, &attr, thread_trampoline, s);
    pthread_attr_destroy(&attr);
    if (status) free(s);
    return status;
}

/* NAME=VALUE lines of <root>/env.txt: switches for the next run
 * without a rebuild (WINE_PS5_SERVER_DIRECT=1, WINEDEBUG=..., ...). */
static char env_text[4096];
static PwWineStartEnv env_pairs[PW_WINE_START_MAX_ENV];
static uint32_t env_count;

static void add_env(const char *name, const char *value)
{
    if (env_count < PW_WINE_START_MAX_ENV) env_pairs[env_count++] = (PwWineStartEnv){ name, value };
}

static void load_env_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    ssize_t got;
    char *line, *next;

    if (fd < 0) return;
    got = read(fd, env_text, sizeof(env_text) - 1);
    close(fd);
    if (got <= 0) return;
    env_text[got] = 0;
    for (line = env_text; line && *line; line = next) {
        char *eq;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (*line == '#' || !(eq = strchr(line, '='))) continue;
        *eq = 0;
        /* A later line replaces an earlier default of the same name. */
        for (uint32_t i = 0; i < env_count; i++)
            if (!strcmp(env_pairs[i].name, line)) { env_pairs[i].value = eq + 1; line = NULL; break; }
        if (line) add_env(line, eq + 1);
        LOG("PW_WINE64 env %s=%s", line ? line : "(replaced)", eq + 1);
    }
}

/* ---- main ------------------------------------------------------------------- */

static const char *const runtime_roots[] = { PW_ROOT "/runtime", "/app0" };

int main(int argc, char **argv)
{
    static char ntdll_dir[512], ntdll_path[512], exe[PW_WINE_LAUNCH_PATH_MAX];
    static const char *wine_argv[3];
    PwWineStartConfig config;
    PwWineStartOps ops = { load_start, module_info, set_env, start_thread };
    ps5log_config logcfg;
    const char *conf_paths[] = { PW_ROOT "/dev.conf" };
    int status, first = 1;
    uint64_t next_alive = 0;

    start_ns = now_ns();
    mkdir(PW_ROOT, 0777);
    mkdir(PW_ROOT "/logs", 0777);
    mkdir(PW_ROOT "/prefix", 0777);
    mkdir(PW_ROOT "/runtime", 0777);
    ps5log_config_defaults(&logcfg);
    ps5log_load_config(conf_paths, 1, &logcfg, NULL);
    ps5log_init(&logcfg, PW_TITLE_ID, "prospero-win-wine64", 0);
    orbislog_klog("PS4WINE start build=" PW_BUILD_ID);
    pw_diagnostics_open(PW_ROOT, PW_BUILD_ID, "hello", 0);
    LOG("PW_WINE64 start build=%s argc=%d argv0=%s netlog=%s:%u", PW_BUILD_ID, argc,
        argc > 0 && argv[0] ? argv[0] : "", logcfg.server, (unsigned)logcfg.port);

    {
        char milestone[16];
        LOG("PW_WINE64 plan %s", pw_ms_select(early_setting("PS4WINE_MILESTONE", milestone, sizeof(milestone))));
    }
    /* path=... from argv[0] on (a LoadExec restart) or argv[1] on; else the plan's program. */
    snprintf(exe, sizeof(exe), "%s", pw_ms_exe());
    for (int i = 0; i < argc; i++) {
        if (argv[i] && !strncmp(argv[i], "path=", 5) && argv[i][5]) snprintf(exe, sizeof(exe), "%s", argv[i] + 5);
    }

    for (size_t i = 0; i < sizeof(runtime_roots) / sizeof(runtime_roots[0]); i++) {
        struct stat st;
        snprintf(ntdll_dir, sizeof(ntdll_dir), "%s" PW_RUNTIME, runtime_roots[i]);
        snprintf(ntdll_path, sizeof(ntdll_path), "%s/ntdll.prx", ntdll_dir);
        if (stat(ntdll_path, &st) == 0) { first = 0; break; }
        LOG("PW_WINE64 runtime not at %s", ntdll_dir);
    }
    LOG("PW_WINE64 ntdll=%s prefix=%s exe=%s found=%d", ntdll_path, PW_ROOT "/prefix", exe, !first);

    {
        PwMsConfig ms = { ntdll_dir, PW_ROOT, PW_BUILD_ID, log_line, emit_line, now_ns_hook };
        pw_ms_start(&ms);
        pw_ms_title_stages();
    }
    if (!pw_ms_wine_should_run()) {
        /* The Wine stage ended or was lost in an earlier launch of this cycle. */
        pw_ms_emit_lines();
        for (;;) sleep(60);
    }
    pw_ms_wine_begin();
    hello_ns = now_ns();
    {
        static PwFdForward forward_stderr = { -1, -1, "WINESERVER" }, forward_stdout = { -1, -1, "STDOUT" };
        static char fd_text[2][16];
        status = capture_fd(2, &forward_stderr);
        LOG("PW_WINE64 fd2_capture status=%d writer=%d", status, forward_stderr.writer);
        status = capture_fd(1, &forward_stdout);
        LOG("PW_WINE64 fd1_capture status=%d writer=%d", status, forward_stdout.writer);
        if (forward_stdout.writer > 2) {
            snprintf(fd_text[0], sizeof(fd_text[0]), "%d", forward_stdout.writer);
            add_env("PW_ORBIS_STDOUT_FD", fd_text[0]);
        }
        if (forward_stderr.writer > 2) {
            snprintf(fd_text[1], sizeof(fd_text[1]), "%d", forward_stderr.writer);
            add_env("PW_ORBIS_STDERR_FD", fd_text[1]);
        }
    }

    memset(&config, 0, sizeof(config));
    config.ntdll_path = ntdll_path;
    config.ntdll_dir = ntdll_dir;
    config.prefix = PW_ROOT "/prefix";
    wine_argv[0] = "wine";
    wine_argv[1] = exe;
    wine_argv[2] = NULL;
    config.argc = 2;
    config.argv = wine_argv;
    config.stack_bytes = 16u << 20;

    load_status = pw_wine_start_load(&start, &config, &ops);
    LOG("PW_WINE64 load status=%d stage=%d module=0x%x segments=%u module_start=%d adopted=%d stats=%d",
        load_status, start.stage, (unsigned)start.module, start.segment_count, start.module_start_result,
        start.adopted != NULL, start.virtual_stats != NULL);
    for (uint32_t i = 0; i < start.segment_count; i++)
        LOG("PW_WINE64 segment %u address=%p size=0x%x prot=%u", i, start.segments[i].address,
            start.segments[i].size, start.segments[i].protection);
    install_early_fault_report();
    if (load_status == PW_OK) {
        void (*set_sink)(void (*)(const char *, size_t)) =
            (void (*)(void (*)(const char *, size_t)))(uintptr_t)pw_prx_lookup(start.descriptor, "__wine_ps5_set_output_sink");
        void (*set_exit)(PwWineExitHook, void *) =
            (void (*)(PwWineExitHook, void *))(uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_set_exit_hook");
        ntdll_setenv = (int (*)(const char *, const char *))(uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_setenv");
        LOG("PW_WINE64 output_sink=%d exit_hook=%d setenv=%d", set_sink != NULL, set_exit != NULL, ntdll_setenv != NULL);
        if (set_sink) set_sink(wine_output_sink);
        if (set_exit) set_exit(exit_hook, NULL);
        {
            /* The user driver's frames (patch 0400's pw_wine_present) to the title's VideoOut. */
            void (*set_present)(int (*)(void *, const void *, uint32_t, uint32_t, uint32_t), void *) =
                (void (*)(int (*)(void *, const void *, uint32_t, uint32_t, uint32_t), void *))(uintptr_t)
                    pw_prx_lookup(start.descriptor, "pw_wine_set_present_sink");
            LOG("PW_WINE64 present_sink=%d", set_present != NULL);
            if (set_present) set_present(pw_ms_present, NULL);
        }
        {
            /* The same cross-check through the kernel's own lookup. */
            void *sym = NULL;
            int rc = sceKernelDlsym(start.module, "__wine_main", &sym);
            LOG("PW_WINE64 dlsym __wine_main rc=0x%x same=%d", (unsigned)rc,
                sym == (void *)(uintptr_t)start.wine_main);
        }
        add_env("WINEDEBUG", "err+all,warn+ps5drv,+loaddll,+process");
        add_env("WINEARCH", "wow64");
        add_env("HOME", PW_ROOT "/prefix");
        add_env("USER", "ps4");
        add_env("WINE_PS5_TRACE_STARTUP", "1");
        add_env("WINE_PRX_DIR", ntdll_dir);
        /* The first run tests the stock server paths only. */
        add_env("WINE_PS5_SERVER_DIRECT", "0");
        add_env("WINE_PS5_MUTEX_SHARED", "0");
        add_env("WINE_PS5_SYNC_SHARED", "0");
        load_env_file(PW_ROOT "/env.txt");
        config.extra_env = env_pairs;
        config.extra_env_count = env_count;
        status = pw_wine_start_environment(&start, &config, &ops);
        LOG("PW_WINE64 environment status=%d", status);
        if (status == PW_OK) {
            status = pw_wine_start_run(&start, &config, &ops);
            LOG("PW_WINE64 run status=%d", status);
        }
        if (status != PW_OK) report("start-failed", "title");
    } else {
        report("load-failed", "title");
    }

    for (;;) {
        uint64_t now = now_ns();
        if (now >= next_alive) {
            uint64_t mem[10] = { 0 }, vm[8] = { 0 };
            unsigned m = start.memory_stats ? start.memory_stats(mem, 10) : 0;
            unsigned v = start.virtual_stats ? start.virtual_stats(vm, 8) : 0;
            LOG("PW_WINE64 alive t=%llu server=%d stage=%d dmem=%llu heap=%llu heap_dmem=%llu vm=%llu/%llu",
                (unsigned long long)((now - start_ns) / 1000000000ull), server_connected, start.stage,
                m ? (unsigned long long)mem[0] : 0ull, m ? (unsigned long long)mem[4] : 0ull,
                m ? (unsigned long long)mem[8] : 0ull, v ? (unsigned long long)vm[0] : 0ull,
                v ? (unsigned long long)vm[1] : 0ull);
            next_alive = now + 1000000000ull;
        }
        pw_diagnostics_tick(now);
        if (!reported && now - hello_ns > (uint64_t)PW_WATCHDOG_S * 1000000000ull) {
            LOG("PW_WINE64 watchdog: no exit after %d s", PW_WATCHDOG_S);
            report("timeout", "watchdog");
        }
        usleep(100000);
    }
}
