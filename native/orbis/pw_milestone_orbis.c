/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The PS4 port's milestone launches in the title (pw_milestone_orbis.h). */
#include "pw_milestone_orbis.h"
#include "pw_stage_orbis.h"
#include "pw_videoout_orbis.h"
#include "pw_pad_orbis.h"
#include "pw_vk_stage_orbis.h"
#include "pw_cursor_probe_orbis.h"
#include "pw_videoout_cursor_orbis.h"
#include "pw_wine_prx.h"
#include "pw_wine_display.h"
#include "pw_wine_exit_orbis.h"
#include "../../wine/wowprospero/wow_stats.h"
#include "pw_hist.h"
#include "pw_game_profile.h"
#include "pw_hid_ps5.h"

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern uint32_t sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags,
                                         const void *option, int *result);
extern int sceKernelGetModuleInfo(int32_t handle, void *info);
extern int sceKernelDlsym(int32_t handle, const char *symbol, void **address);
extern int sceKernelGetModuleList(int32_t *handles, size_t size, size_t *count);
extern int sceKernelAvailableFlexibleMemorySize(size_t *size);
/* libkernel's creator, under the title's own pthread layer (orbis-compat's
 * pthread_create, which would raise or keep the stack itself). */
extern int scePthreadCreate(pthread_t *thread, const pthread_attr_t *attr, void *(*entry)(void *), void *arg,
                            const char *name);
/* libkernel's SceKernelVirtualQueryInfo (OpenOrbis _types/kernel.h): the
 * region, then what backs it. Flags 1: the region at or after the address. */
typedef struct VqInfo {
    void *start, *end;
    int64_t offset;
    int32_t protection, type;
    uint32_t kind;           /* flexible, direct, stack, pooled, committed: bits 0-4 */
    char name[32];
} VqInfo;
extern int sceKernelVirtualQuery(const void *address, int flags, VqInfo *info, size_t size);

typedef void (*StageFn)(char *fields, size_t size);
/* A stage: its name, time limit, what it runs (NULL: the Wine stage, last)
 * and what its fields say when it faulted and was recovered. */
typedef struct PwMsStage { const char *name; unsigned limit_s; StageFn run; const char *fault_fields; } PwMsStage;
typedef struct PwMsPlan {
    const char *name;
    const PwMsStage *stages;
    unsigned count;
    const char *exe;
    int c_line;
    void (*lines)(void);
} PwMsPlan;

static const PwMsPlan *plan;
static PwMsConfig cfg;
static PwStageRun run;
static char progress_path[256];
/* setjmp/longjmp, not sigsetjmp/siglongjmp: libc.a has no sigsetjmp, so the
 * link took libkernel's (FreeBSD's jmp_buf) beside musl's siglongjmp, and the
 * first recovery jumped to garbage (e1 launch 1: rip 0x20, rsp 0x1). musl's
 * pair is one implementation; the handlers run with SA_NODEFER, so there is
 * no blocked signal to restore. */
static jmp_buf stage_jump;
static volatile int stage_active = -1;
static volatile int recovering;
static volatile uint64_t stage_deadline;
static volatile int lines_emitted;
static uint64_t start_ns;
static PwVideoOutOrbis video;
static int video_open;
/* Phase p: the timestamps and CPU reads run in the p plans only (bench_plan);
 * their present path and flip (p_path, p_async: see pw_ms_present). */
enum { P_PATH_SCALE, P_PATH_VO720, P_PATH_VO1080 };
static const char *const p_path_names[] = { "scale", "vo720", "vo1080" };
static int bench_plan, p_path, p_async;
static int native_plan(void);   /* p2: 32-bit code on the hardware WoW64 backend */
static void p5_lines(void);
static void p5w_lines(void);
static void p6_lines(void);
static void hold_sample(int at);
static const char *ntdll_symbol(const char *name, void **address);
static void pause_ms(unsigned ms);
/* Milestone g (docs/ps4/HANDOFF-g.md in ps4-wine). */
static int g_plan(void);
static int g1_plans(void);
static void *g_script(void *arg);
static void g1n_lines(void);
static void g1m_lines(void);
static void g1_lines(void);
static void g_lines(void);
/* session_s counts from here when set (g: from the gameplay mark), else from the title's start. */
static uint64_t session_base_ns;

/* ---- the progress file, through stdio ----------------------------------- */

static long file_read(void *context, const char *path, char *buf, size_t size)
{
    FILE *file = fopen(path, "rb");
    size_t got;
    (void)context;
    if (!file) return -1;
    got = fread(buf, 1, size - 1, file);
    fclose(file);
    buf[got] = 0;
    return (long)got;
}

/* One line, flushed and synced before the caller goes on (the milestone a
 * probe measured that a synced file on /data survives a crash). */
static int file_append(void *context, const char *path, const char *line)
{
    FILE *file = fopen(path, "ab");
    int ok;
    (void)context;
    if (!file) return -1;
    ok = fputs(line, file) >= 0 && fputc('\n', file) != EOF && fflush(file) == 0 && fsync(fileno(file)) == 0;
    return fclose(file) == 0 && ok ? 0 : -1;
}

static int file_rename(void *context, const char *from, const char *to)
{
    (void)context;
    unlink(to);
    return rename(from, to);
}

static void stage_log(void *context, const char *line)
{
    (void)context;
    cfg.emit(line);
}

static const PwStageOps stage_ops = { file_read, file_append, file_rename, stage_log, NULL };

static void step(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void step(const char *format, ...)
{
    char line[512];
    va_list ap;
    int n = snprintf(line, sizeof(line), "PW_MS step ");
    va_start(ap, format);
    vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
    va_end(ap);
    cfg.log(line);
}

/* ---- modules -------------------------------------------------------------- */

typedef struct Module {
    int32_t handle;
    char name[PW_PRX_MAX_NAME];
    PwPrxSegment segments[PW_PRX_MAX_SEGMENTS];
    uint32_t count;
    uint64_t low, high;
} Module;

static int module_info(int32_t handle, Module *m)
{
    uint8_t info[PW_PRX_MODULE_INFO_BYTES];
    uint64_t size_word = sizeof(info);

    memset(m, 0, sizeof(*m));
    m->handle = handle;
    memset(info, 0, sizeof(info));
    memcpy(info, &size_word, sizeof(size_word));
    if (sceKernelGetModuleInfo(handle, info) != 0) return -1;
    memcpy(info, &size_word, sizeof(size_word));
    if (pw_prx_parse_module_info(info, m->name, m->segments, &m->count) != PW_PRX_OK || !m->count) return -1;
    m->low = UINT64_MAX;
    for (uint32_t i = 0; i < m->count; i++) {
        uint64_t a = (uint64_t)(uintptr_t)m->segments[i].address;
        if (a < m->low) m->low = a;
        if (a + m->segments[i].size > m->high) m->high = a + m->segments[i].size;
    }
    return 0;
}

const char *pw_orbis_module_of(uint64_t address, char *out, size_t size)
{
    int32_t handles[64];
    size_t count = 0;

    snprintf(out, size, "?");
    if (sceKernelGetModuleList(handles, sizeof(handles) / sizeof(handles[0]), &count) != 0) return out;
    for (size_t i = 0; i < count; i++) {
        Module m;
        if (module_info(handles[i], &m)) continue;
        for (uint32_t s = 0; s < m.count; s++) {
            uint64_t base = (uint64_t)(uintptr_t)m.segments[s].address;
            if (address >= base && address < base + m.segments[s].size) {
                snprintf(out, size, "%s+0x%llx", m.name, (unsigned long long)(address - base));
                return out;
            }
        }
    }
    return out;
}

/* Whether a loaded module's name is NAME: the module list names them with
 * the extension ("ntdll.prx", measured in the second e2 launch), so NAME
 * and NAME.prx both match. Before that, the plain comparison never matched
 * an app PRX: d1's early_autoload=0 came from it and does not show that the
 * loader leaves a NEEDED app module unloaded. */
static int module_named(const char *module, const char *name)
{
    size_t n = strlen(name);
    return !strncmp(module, name, n) && (!module[n] || !strcmp(module + n, ".prx"));
}

/* Whether a module of this name is loaded (the loader may load a NEEDED
 * application module by itself). */
static int module_loaded(const char *name)
{
    int32_t handles[64];
    size_t count = 0;
    if (sceKernelGetModuleList(handles, sizeof(handles) / sizeof(handles[0]), &count) != 0) return -1;
    for (size_t i = 0; i < count; i++) {
        Module m;
        if (!module_info(handles[i], &m) && module_named(m.name, name)) return 1;
    }
    return 0;
}

/* The handle of a loaded module of this name, or -1. */
static int32_t module_handle(const char *name)
{
    int32_t handles[64];
    size_t count = 0;
    if (sceKernelGetModuleList(handles, sizeof(handles) / sizeof(handles[0]), &count) != 0) return -1;
    for (size_t i = 0; i < count; i++) {
        Module m;
        if (!module_info(handles[i], &m) && module_named(m.name, name)) return handles[i];
    }
    return -1;
}

static int32_t load(const char *name, int *result)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.prx", cfg.module_dir, name);
    *result = 0;
    return (int32_t)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, result);
}

/* pw_bind_lib, loaded by this launch if an earlier one did it (a resumed
 * launch is a new process). */
static int32_t bind_lib(void)
{
    int result;
    int32_t handle = load("pw_bind_lib", &result);
    return handle > 0 ? handle : -1;
}

/* ---- import slots ------------------------------------------------------------ */

typedef struct Slots {
    unsigned total, bound, zero, lazy, other, nosym;
    char first[96];      /* the first slot that is not bound, "symbol:kind" */
    uint64_t first_value;
    const char *watch;   /* one symbol whose own state is wanted (NULL: none) */
    char watch_state[16];
    int table;           /* 0 when the table could not be read */
} Slots;

static void slot_note(Slots *s, const char *symbol, const char *kind, uint64_t value)
{
    if (!s->first[0]) {
        snprintf(s->first, sizeof(s->first), "%s:%s", symbol, kind);
        s->first_value = value;
    }
}

/* An exporter of a slot table's "from" column (NULL name: every slot). */
typedef struct Exporter { const char *name; int32_t handle; } Exporter;

/* Each slot of <name>.bind.txt against what sceKernelDlsym says its
 * exporter (by the slot's "from") exports: bound (equal), zero, lazy (still
 * inside the importer: a PLT entry), other; nosym when the look-up fails or
 * no exporter of that name was given. */
static void check_slots_from(const char *name, const Module *importer, const Exporter *exporters, size_t count,
                             Slots *s, const char *watch)
{
    static char text[32768];
    char path[512];
    uint64_t first_vaddr = 0;
    FILE *file;
    size_t got;

    memset(s, 0, sizeof(*s));
    s->watch = watch;
    snprintf(s->watch_state, sizeof(s->watch_state), "absent");
    snprintf(path, sizeof(path), "%s/%s.bind.txt", cfg.module_dir, name);
    if (!(file = fopen(path, "rb"))) return;
    got = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[got] = 0;
    s->table = 1;
    for (char *line = text, *next; line && *line; line = next) {
        unsigned long long offset;
        char kind[16], from[64], symbol[128];
        const char *state;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (sscanf(line, "first_vaddr %llx", &offset) == 1) { first_vaddr = offset; continue; }
        if (sscanf(line, "slot %llx %15s %63s %127s", &offset, kind, from, symbol) != 4) continue;
        {
            uint64_t address = importer->low + offset - first_vaddr, value = 0;
            void *expected = NULL;
            int32_t exporter = -1;
            for (size_t i = 0; i < count && exporter < 0; i++)
                if (!exporters[i].name || !strcmp(exporters[i].name, from)) exporter = exporters[i].handle;
            s->total++;
            if (address < importer->low || address + 8 > importer->high) {
                s->other++;
                state = "outside";
            } else {
                memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
                if (exporter < 0 || sceKernelDlsym(exporter, symbol, &expected) != 0 || !expected) s->nosym++;
                if (expected && value == (uint64_t)(uintptr_t)expected) { s->bound++; state = "bound"; }
                else if (!value) { s->zero++; state = "zero"; }
                else if (value >= importer->low && value < importer->high) { s->lazy++; state = "lazy"; }
                else if (expected) { s->other++; state = "other"; }
                else state = "nosym";
            }
            if (strcmp(state, "bound")) slot_note(s, symbol, state, value);
            if (watch && !strcmp(symbol, watch)) snprintf(s->watch_state, sizeof(s->watch_state), "%s", state);
        }
    }
}

static void check_slots(const char *name, const Module *importer, int32_t exporter, Slots *s, const char *watch)
{
    const Exporter one = { NULL, exporter };
    check_slots_from(name, importer, &one, 1, s, watch);
}

static const char *slot_state(const Slots *s)
{
    if (!s->table) return "notable";
    if (!s->total) return "none";
    if (s->bound == s->total) return "bound";
    if (s->zero == s->total) return "zero";
    if (s->lazy == s->total) return "lazy";
    return "mixed";
}

static void slot_step(const char *what, const Slots *s)
{
    char where[120];
    step("%s slots=%s total=%u bound=%u zero=%u lazy=%u other=%u nosym=%u first=%s value=0x%llx in=%s%s%s",
         what, slot_state(s), s->total, s->bound, s->zero, s->lazy, s->other, s->nosym,
         s->first[0] ? s->first : "-", (unsigned long long)s->first_value,
         s->first_value ? pw_orbis_module_of(s->first_value, where, sizeof(where)) : "-",
         s->watch ? " watch=" : "", s->watch ? s->watch_state : "");
}

/* ---- d1: binding, VideoOut, the DS4 --------------------------------------------------------------- */

static void stage_bind_early(char *fields, size_t size)
{
    int result, autoload;
    int32_t handle = load("pw_bind_early", &result);
    Slots s;

    step("bind_early load=0x%x result=%d", (unsigned)handle, result);
    if (handle <= 0) {
        snprintf(fields, size, "early_load=0x%x order_needed=1", (unsigned)handle);
        return;
    }
    autoload = module_loaded("pw_bind_lib");
    memset(&s, 0, sizeof(s));
    if (autoload == 1) {
        Module m;
        int32_t lib = bind_lib();
        if (!module_info(handle, &m) && lib > 0) check_slots("pw_bind_early", &m, lib, &s, NULL);
        slot_step("bind_early", &s);
    }
    snprintf(fields, size, "early_load=ok early_autoload=%d early_slots=%s order_needed=%d", autoload,
             autoload == 1 ? slot_state(&s) : "-", autoload == 1 && s.bound == s.total && s.total ? 0 : 1);
}

static void stage_bind_lib(char *fields, size_t size)
{
    int result;
    int32_t handle = load("pw_bind_lib", &result);
    void *add = NULL, *name = NULL;
    const PwPrxDescriptor *descriptor = NULL;
    Module m;
    int ok = 0, desc = 0;

    step("bind_lib load=0x%x result=%d", (unsigned)handle, result);
    if (handle <= 0) { snprintf(fields, size, "lib=fail lib_load=0x%x", (unsigned)handle); return; }
    if (!module_info(handle, &m) && pw_prx_find_descriptor(m.segments, m.count, &descriptor) == PW_PRX_OK)
        desc = pw_prx_lookup(descriptor, "pw_bind_add") != NULL;
    if (!sceKernelDlsym(handle, "pw_bind_add", &add) && !sceKernelDlsym(handle, "pw_bind_name", &name) && add && name) {
        const char *text = ((const char *(*)(void))name)();
        ok = ((int (*)(int, int))add)(40, 2) == 42 && text && !strcmp(text, "pw_bind_lib");
    }
    snprintf(fields, size, "lib=%s lib_load=ok lib_desc=%d", ok ? "ok" : "fail", desc);
}

static void stage_bind_import(char *fields, size_t size)
{
    int result, ok = 0;
    int32_t lib = bind_lib(), handle;
    void *entry = NULL;
    Module m;
    Slots before, after;
    char state_before[16];

    if (lib <= 0) { snprintf(fields, size, "import=fail import_why=nolib"); return; }
    handle = load("pw_bind_use", &result);
    step("bind_import load=0x%x result=%d", (unsigned)handle, result);
    if (handle <= 0) { snprintf(fields, size, "import=fail import_load=0x%x", (unsigned)handle); return; }
    if (module_info(handle, &m)) { snprintf(fields, size, "import=fail import_why=moduleinfo"); return; }
    check_slots("pw_bind_use", &m, lib, &before, NULL);
    slot_step("bind_import before", &before);
    snprintf(state_before, sizeof(state_before), "%s", slot_state(&before));
    /* A zero slot would fault the call: say so instead. */
    if (before.zero || !before.total) {
        snprintf(fields, size, "import=fail import_slots=%s import_load=ok", state_before);
        return;
    }
    if (sceKernelDlsym(handle, "pw_bind_use_run", &entry) || !entry) {
        snprintf(fields, size, "import=fail import_slots=%s import_why=nosym", state_before);
        return;
    }
    ok = ((int (*)(void))entry)() == 1;
    check_slots("pw_bind_use", &m, lib, &after, NULL);
    slot_step("bind_import after", &after);
    snprintf(fields, size, "import=%s import_slots=%s import_after=%s import_load=ok", ok ? "ok" : "fail",
             state_before, slot_state(&after));
}

static const void *lookup_in(void *context, const char *name)
{
    return pw_prx_lookup((const PwPrxDescriptor *)context, name);
}

static void stage_bind_thunk(char *fields, size_t size)
{
    int result, missing, ok;
    int32_t lib = bind_lib(), handle;
    const PwPrxDescriptor *lib_desc = NULL, *thunk_desc = NULL;
    int (*bind)(const void *(*)(void *, const char *), void *);
    int (*entry)(void);
    Module m;

    if (lib <= 0 || module_info(lib, &m) || pw_prx_find_descriptor(m.segments, m.count, &lib_desc) != PW_PRX_OK) {
        snprintf(fields, size, "thunk=fail thunk_why=nolib");
        return;
    }
    handle = load("pw_bind_thunk", &result);
    step("bind_thunk load=0x%x result=%d", (unsigned)handle, result);
    if (handle <= 0 || module_info(handle, &m) ||
        pw_prx_find_descriptor(m.segments, m.count, &thunk_desc) != PW_PRX_OK) {
        snprintf(fields, size, "thunk=fail thunk_load=0x%x", (unsigned)handle);
        return;
    }
    /* Only the PRXDESC1 tables: no loader look-up on this path. */
    bind = (int (*)(const void *(*)(void *, const char *), void *))(uintptr_t)pw_prx_lookup(thunk_desc, "pw_prx_bind_imports");
    entry = (int (*)(void))(uintptr_t)pw_prx_lookup(thunk_desc, "pw_bind_thunk_run");
    if (!bind || !entry) { snprintf(fields, size, "thunk=fail thunk_why=nodesc"); return; }
    missing = bind(lookup_in, (void *)lib_desc);
    ok = !missing && entry() == 1;
    snprintf(fields, size, "thunk=%s thunk_missing=%d", ok ? "ok" : "fail", missing);
}

static void stage_bind_data(char *fields, size_t size)
{
    int result, value;
    int32_t lib = bind_lib(), handle;
    void *counter = NULL, *read = NULL;
    Module m;
    Slots s;

    if (lib <= 0 || sceKernelDlsym(lib, "pw_bind_counter", &counter) || !counter) {
        snprintf(fields, size, "data=fail data_why=%s", lib <= 0 ? "nolib" : "nosym");
        return;
    }
    handle = load("pw_bind_data", &result);
    step("bind_data load=0x%x result=%d", (unsigned)handle, result);
    if (handle <= 0 || module_info(handle, &m)) { snprintf(fields, size, "data=fail data_load=0x%x", (unsigned)handle); return; }
    check_slots("pw_bind_data", &m, lib, &s, NULL);
    slot_step("bind_data", &s);
    if (s.zero || !s.total) { snprintf(fields, size, "data=fail data_slots=%s", slot_state(&s)); return; }
    if (sceKernelDlsym(handle, "pw_bind_data_read", &read) || !read) {
        snprintf(fields, size, "data=fail data_slots=%s data_why=nosym", slot_state(&s));
        return;
    }
    value = ((int (*)(void))read)();
    snprintf(fields, size, "data=%s data_slots=%s data_value=%d", value == *(const int *)counter ? "ok" : "fail",
             slot_state(&s), value);
}

static void stage_w32u_bind(char *fields, size_t size)
{
    int result;
    int32_t ntdll = load("ntdll", &result), win32u;
    void *init = NULL;
    Module m;
    Slots s;

    step("w32u_bind ntdll load=0x%x result=%d", (unsigned)ntdll, result);
    if (ntdll <= 0) { snprintf(fields, size, "w32u_load=nontdll w32u_bound=-"); return; }
    win32u = load("win32u", &result);
    step("w32u_bind win32u load=0x%x result=%d", (unsigned)win32u, result);
    if (win32u <= 0) { snprintf(fields, size, "w32u_load=0x%x w32u_bound=-", (unsigned)win32u); return; }
    if (module_info(win32u, &m)) { snprintf(fields, size, "w32u_load=ok w32u_bound=nomoduleinfo"); return; }
    check_slots("win32u", &m, ntdll, &s, "__wrap_dlsym");
    slot_step("w32u_bind", &s);
    sceKernelDlsym(win32u, "__wine_unix_lib_init", &init);
    snprintf(fields, size, "w32u_load=ok w32u_bound=%u/%u w32u_zero=%u w32u_lazy=%u w32u_other=%u w32u_nosym=%u "
             "w32u_first=%s w32u_dlsym=%s w32u_init=%d", s.bound, s.total, s.zero, s.lazy, s.other, s.nosym,
             s.first[0] ? s.first : "-", s.watch_state, init != NULL);
}

static void stage_videoout(char *fields, size_t size)
{
    char shot[512];
    uint64_t t0;
    int rc, saved;
    unsigned flips = 0;

    snprintf(shot, sizeof(shot), "%s/shot.raw", cfg.root);
    unlink(shot);
    if ((rc = pw_videoout_orbis_open(&video))) {
        snprintf(fields, size, "videoout=fail vo_failed=%s:0x%x fb=-", video.failed, (unsigned)rc);
        return;
    }
    step("videoout open handle=%d buffers=%p,%p", video.handle, (void *)video.buffers[0], (void *)video.buffers[1]);
    for (unsigned i = 0; i < PW_VIDEOOUT_ORBIS_BUFFERS; i++) pw_videoout_orbis_test_pattern(video.buffers[i]);
    t0 = cfg.now_ns();
    for (; flips < 120; flips++)
        if ((rc = pw_videoout_orbis_flip(&video))) break;
    {
        uint64_t elapsed = cfg.now_ns() - t0;
        saved = flips == 120 ? pw_videoout_orbis_save(&video, shot) : -1;
        snprintf(fields, size, "videoout=%s flips=%u flip_hz=%.1f%s%s fb=%s", flips == 120 ? "ok" : "fail", flips,
                 elapsed ? flips * 1e9 / (double)elapsed : 0.0, rc ? " vo_failed=" : "", rc ? video.failed : "",
                 saved == 0 ? "ok" : "fail");
    }
}

static void stage_pad(char *fields, size_t size)
{
    PwPadOrbis pad;
    PwPadOrbisState state;
    uint32_t seen = 0;
    unsigned reads = 0, failed_reads = 0;
    int bytes, cross = 0, rc;
    uint8_t connected = 0;
    uint64_t end;

    if ((rc = pw_pad_orbis_open(&pad))) {
        snprintf(fields, size, "pad=none pad_failed=%s:0x%x pad_init=0x%x pad_cross=0", pad.failed, (unsigned)rc,
                 (unsigned)pad.init_status);
        return;
    }
    bytes = pw_pad_orbis_measure(&pad);
    step("pad open user=0x%x handle=%d init=0x%x bytes=%d (press cross)", (unsigned)pad.user, pad.handle,
         (unsigned)pad.init_status, bytes);
    end = cfg.now_ns() + 10000000000ull;
    while (cfg.now_ns() < end && !cross) {
        struct timespec pause = { 0, 16000000 };
        memset(&state, 0, sizeof(state));
        if (pw_pad_orbis_read(&pad, &state)) failed_reads++;
        else {
            reads++;
            seen |= state.buttons;
            connected = ((const uint8_t *)&state)[PW_PAD_ORBIS_CONNECTED_AT];
            cross = (state.buttons & PW_PAD_ORBIS_CROSS) != 0;
        }
        nanosleep(&pause, NULL);
    }
    snprintf(fields, size, "pad=ok pad_cross=%d pad_bytes=%d pad_connected=%u pad_buttons=0x%x pad_reads=%u "
             "pad_read_errors=%u pad_user=0x%x", cross, bytes, connected, (unsigned)seen, reads, failed_reads,
             (unsigned)pad.user);
}


/* ---- d2: libfreetype's slots, the display ------------------------------------ */

static void stage_ft_bind(char *fields, size_t size)
{
    int result;
    int32_t ntdll = load("ntdll", &result), ft;
    void *init = NULL;
    Module m;
    Slots s;

    step("ft_bind ntdll load=0x%x result=%d", (unsigned)ntdll, result);
    if (ntdll <= 0) { snprintf(fields, size, "ft_load=nontdll ft_bound=-"); return; }
    ft = load("libfreetype", &result);
    step("ft_bind libfreetype load=0x%x result=%d", (unsigned)ft, result);
    if (ft <= 0) { snprintf(fields, size, "ft_load=0x%x ft_bound=-", (unsigned)ft); return; }
    if (module_info(ft, &m)) { snprintf(fields, size, "ft_load=ok ft_bound=nomoduleinfo"); return; }
    check_slots("libfreetype", &m, ntdll, &s, NULL);
    slot_step("ft_bind", &s);
    /* Its exports are found through its PRXDESC1 table by Wine's dlsym; the
     * kernel's look-up is the cross-check. FreeType itself runs in the gdi
     * stage, after ntdll's start (its malloc is ntdll's heap). */
    sceKernelDlsym(ft, "FT_Init_FreeType", &init);
    snprintf(fields, size, "ft_load=ok ft_bound=%u/%u ft_first=%s ft_init_sym=%d", s.bound, s.total,
             s.first[0] ? s.first : "-", init != NULL);
}

/* The prefix's Fonts directory listed by the title: opendir and readdir of
 * the same static musl as Wine's ntdll, which finds fonts only by listing
 * C:\windows\Fonts (font.c load_directory_fonts). The first d2 launch got
 * no font at all with the 13 files in place. */
static void stage_fonts_dir(char *fields, size_t size)
{
    char path[512];
    unsigned entries = 0, ttf = 0;
    struct dirent *de;
    DIR *dir;
    int err;

    snprintf(path, sizeof(path), "%s/prefix/drive_c/windows/Fonts", cfg.root);
    errno = 0;
    if (!(dir = opendir(path))) {
        snprintf(fields, size, "fontdir=fail fontdir_errno=%d", errno);
        return;
    }
    errno = 0;
    while ((de = readdir(dir))) {
        size_t n = strlen(de->d_name);
        entries++;
        if (n > 4 && !strcmp(de->d_name + n - 4, ".ttf")) ttf++;
        if (entries <= 3) step("fonts_dir entry %u %s", entries, de->d_name);
    }
    err = errno;
    closedir(dir);
    {
        /* win32u maps each font file read-only and private (freetype.c
         * unix_face_create): whether that works for a file on /data. */
        struct stat st;
        char tahoma[600], map[48];
        int fd;
        snprintf(tahoma, sizeof(tahoma), "%s/tahoma.ttf", path);
        snprintf(map, sizeof(map), "noopen:%d", errno);
        if ((fd = open(tahoma, O_RDONLY)) >= 0) {
            if (fstat(fd, &st) == 0 && st.st_size >= 4) {
                const uint8_t *data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
                if (data == MAP_FAILED) snprintf(map, sizeof(map), "fail:%d", errno);
                else {
                    snprintf(map, sizeof(map), "ok:%02x%02x%02x%02x", data[0], data[1], data[2], data[3]);
                    munmap((void *)data, (size_t)st.st_size);
                }
            } else snprintf(map, sizeof(map), "nostat:%d", errno);
            close(fd);
        }
        snprintf(fields, size, "fontdir=%s fontdir_entries=%u fontdir_ttf=%u fontdir_errno=%d tahoma_stat=%lld "
                 "tahoma_mmap=%s", ttf ? "ok" : "empty", entries, ttf, err,
                 stat(tahoma, &st) ? -1ll : (long long)st.st_size, map);
    }
}

static void stage_display(char *fields, size_t size)
{
    char shot[512];
    int rc;

    PwVideoOutOrbisMode mode = { PW_VIDEOOUT_ORBIS_WIDTH, PW_VIDEOOUT_ORBIS_HEIGHT, PW_VIDEOOUT_ORBIS_WIDTH, 0,
                                 PW_VIDEOOUT_ORBIS_BUFFERS, 0 };
    const char *tried = "";

    snprintf(shot, sizeof(shot), "%s/shot.raw", cfg.root);
    unlink(shot);
    if (bench_plan) {
        mode.buffers = p_async ? 3 : 2;
        mode.async = p_async;
        /* 960x540 was refused (0x80290005 at pitch 960 and 1024, LOG.md
         * 2026-11-01 18:21); 1280x720 registered in milestone a. */
        if (p_path == P_PATH_VO720) {
            mode.width = mode.pitch = 1280;
            mode.height = 720;
            mode.bgra = 1;
        } else if (p_path == P_PATH_VO1080) {
            mode.bgra = 1;
        }
    }
    rc = pw_videoout_orbis_open_mode(&video, &mode);
    if (rc) {
        snprintf(fields, size, "display=fail vo_failed=%s:0x%x vo_register=%s%s", video.failed, (unsigned)rc,
                 video.failed && !strcmp(video.failed, "sceVideoOutRegisterBuffers") ? "err" : "-", tried);
        return;
    }
    video_open = 1;
    if ((rc = pw_videoout_orbis_flip(&video))) {
        snprintf(fields, size, "display=fail vo_failed=%s:0x%x vo_register=ok%s", video.failed, (unsigned)rc, tried);
        return;
    }
    step("display open handle=%d %ux%u pitch=%u %s buffers=%u %s", video.handle, video.mode.width,
         video.mode.height, video.mode.pitch, video.mode.bgra ? "A8R8G8B8_SRGB" : "A8B8G8R8_SRGB",
         video.mode.buffers, video.mode.async ? "async" : "sync");
    snprintf(fields, size, "display=ok vo_register=ok vo_size=%ux%u vo_pitch=%u vo_format=%s vo_buffers=%u%s",
             video.mode.width, video.mode.height, video.mode.pitch, video.mode.bgra ? "a8r8g8b8" : "a8b8g8r8",
             video.mode.buffers, tried);
}

/* p3/p4: a desktop-sized frame through the plan's own present path for 5 s,
 * before Wine, for the HDMI check: black, a green square on the centre and
 * an orange one on (w/8, h/8), alpha 0 as the user driver leaves it. On the
 * 960x540 desktop (scale, vo1080): green 100x100 on 480,270, orange 32x32 on
 * 120,67; on vo720's 1280x720: 132x132 on 640,360 and 42x42 on 160,90. Shown
 * full-screen (2x, or VideoOut's 1.5x) they land on HDMI 960,540 and 240,135
 * (d1's pixels); a buffer shown unscaled, in the corner or centred, fails the
 * orange one. */
enum { PATTERN_MS = 5000, PATTERN_EVERY_MS = 100 };

static void pattern_square(uint32_t *pixels, uint32_t width, int cx, int cy, int half, uint32_t bgrx)
{
    for (int y = cy - half; y < cy + half; y++)
        for (int x = cx - half; x < cx + half; x++) pixels[(size_t)y * width + (size_t)x] = bgrx;
}

static void stage_pattern(char *fields, size_t size)
{
    const uint32_t w = p_path == P_PATH_VO720 ? 1280 : 960, h = p_path == P_PATH_VO720 ? 720 : 540;
    uint32_t *img = calloc((size_t)w * h, 4);
    int shown = 0, failed = 0;
    uint64_t t0;

    if (!img || !video_open) {
        snprintf(fields, size, "vo_flip=%s", img ? "nodisplay" : "nomemory");
        free(img);
        return;
    }
    pattern_square(img, w, (int)w / 2, (int)h / 2, (int)(w * 50 / 960), 0x0000ff00u);   /* green: B=0 G=255 R=0 */
    pattern_square(img, w, (int)w / 8, (int)h / 8, (int)(w * 16 / 960), 0x00ff8000u);   /* orange: B=0 G=128 R=255 */
    step("p pattern on screen for %d ms: %ux%u, green on %u,%u, orange on %u,%u (HDMI 960,540 and 240,135 when "
         "shown full-screen)", PATTERN_MS, w, h, w / 2, h / 2, w / 8, h / 8);
    t0 = cfg.now_ns();
    while (cfg.now_ns() - t0 < (uint64_t)PATTERN_MS * 1000000ull) {
        if (pw_ms_present(NULL, img, w, h, w * 4)) failed++;
        else shown++;
        pause_ms(PATTERN_EVERY_MS);
    }
    step("p pattern off after %d presents (%d refused)", shown, failed);
    snprintf(fields, size, "vo_flip=%s pattern_presents=%d", failed || video.flip_errors ? "fail" : "ok", shown);
    free(img);
}

/* ---- the present sink ------------------------------------------------------------ */

static pthread_mutex_t present_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t frames, drawn;
static uint32_t frame_width, frame_height;
static uint32_t *last_drawn;   /* the last frame with any pixel drawn, as the driver's BGRA (frame_width x frame_height) */
static uint32_t cleared[PW_VIDEOOUT_ORBIS_MAX_BUFFERS][2];   /* the frame size each buffer's border was cleared for */
/* Latency: from the first pad event not yet shown to the next present, and each present's own time. */
static volatile uint64_t input_pending_ns;
static uint64_t lat_n, lat_sum_ns, lat_max_ns, present_n, present_sum_ns;
static uint32_t present_scale, present_x0, present_y0;   /* desktop -> 1920x1080 */
static int fb_saved = -1;      /* -1 not yet, 0 saved, else failed */
static uint64_t gdi_dropped;   /* frames the driver sent after the display was handed over */

/* Phase p's measurements (docs/ps4/HANDOFF-p.md in ps4-wine), per present
 * while bench_on, under present_lock. guest: the previous present's exit to
 * this one's entry (translated code, Wine, the guest's own waits); copy: the
 * swizzle and scale; keep: the last_drawn copy; flip: the flip call; frame:
 * entry to entry; present: entry to exit (with the lock wait). guest_cpu:
 * the presenting thread's CPU time over the guest window (getrusage
 * RUSAGE_THREAD from libkernel; orbis-compat answers every CPU-time
 * clock_gettime id with the monotonic clock, so that would be wall time). */
static int bench_on;
static PwHist hist_guest, hist_copy, hist_keep, hist_flip, hist_frame, hist_present, hist_guest_cpu;
static uint64_t bench_presents, bench_noted, bench_flushes, bench_full, bench_composed;
static uint64_t bench_dirty_sum, bench_dirty_max;
static uint64_t prev_enter_ns, prev_exit_ns, prev_exit_cpu_us;
static pthread_t prev_thread;
static int prev_valid, thread_cpu_errno = -1;   /* -1: not read yet, 0 ok, else getrusage's errno */
static uint64_t thread_cpu_other;      /* samples skipped: a different thread presented */
/* The p plans' present path and flip (pw_ms_start, env.txt). p_path:
 * scale, today's 1080p A8B8G8R8 with the CPU's swizzle and integer scale;
 * vo720, 1280x720 A8R8G8B8_SRGB buffers on a 1280x720 desktop, one row copy,
 * VideoOut asked to scale to the 1080p output (what p3 measures); vo1080,
 * 1920x1080 A8R8G8B8_SRGB, no swizzle, the 2x scale as a widened row and its
 * copy. In both vo paths the driver's B,G,R,X is the buffer's memory order,
 * and the keep copy moves to save time (the readback comes from the scan-out
 * buffer shown last). p_async: three buffers and a flip that returns once
 * submitted, the wait moved to the next present's start and only for a free
 * buffer (p4). */
static uint32_t buf_scale, buf_x0, buf_y0;   /* where a vo path put the frame in its buffer */
static uint32_t widened[PW_VIDEOOUT_ORBIS_WIDTH];   /* one scaled row (vo1080), under present_lock */
static int64_t shown_arg0;     /* the last completed flip's argument at the segment's start */
static uint64_t slot_waits0;
/* The driver's note for the next present (pw_ms_flush), consumed by it. */
static int note_pending;
static uint32_t note_dirty, note_screen, note_composed;

/* The calling thread's user + system time in us; 0 and the errno kept on failure. */
static uint64_t thread_cpu_us(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_THREAD, &ru)) {
        thread_cpu_errno = errno ? errno : -2;
        return 0;
    }
    if (thread_cpu_errno == -1) thread_cpu_errno = 0;
    return (uint64_t)ru.ru_utime.tv_sec * 1000000ull + (uint64_t)ru.ru_utime.tv_usec +
           (uint64_t)ru.ru_stime.tv_sec * 1000000ull + (uint64_t)ru.ru_stime.tv_usec;
}

/* present_lock held */
static void bench_reset(void)
{
    pw_hist_reset(&hist_guest);
    pw_hist_reset(&hist_copy);
    pw_hist_reset(&hist_keep);
    pw_hist_reset(&hist_flip);
    pw_hist_reset(&hist_frame);
    pw_hist_reset(&hist_present);
    pw_hist_reset(&hist_guest_cpu);
    bench_presents = bench_noted = bench_flushes = bench_full = bench_composed = 0;
    bench_dirty_sum = bench_dirty_max = 0;
    thread_cpu_other = 0;
}

/* ntdll's flush sink (pw_wine_set_flush_sink, patch 0490): what the next
 * present changed, told by the driver just before it on the same thread. */
void pw_ms_flush(void *context, uint32_t dirty_px, uint32_t screen_px, uint32_t composed)
{
    (void)context;
    pthread_mutex_lock(&present_lock);
    note_pending = 1;
    note_dirty = dirty_px;
    note_screen = screen_px;
    note_composed = composed;
    pthread_mutex_unlock(&present_lock);
}

/* BGRA from the driver, integer-scaled and centred on the 1920x1080
 * buffer as R,G,B,A. The last frame with any pixel drawn is copied aside
 * and saved as the framebuffer readback when the program ends: the driver
 * presents only on changes (the first d2 launch sent 2 frames for a window
 * shown 2 s), so a frame picked by time may never come. */
int pw_ms_present(void *context, const void *bgra, uint32_t width, uint32_t height, uint32_t stride)
{
    const uint32_t W = PW_VIDEOOUT_ORBIS_WIDTH, H = PW_VIDEOOUT_ORBIS_HEIGHT;
    uint32_t scale, x0, y0, *dst;
    uint64_t t0 = cfg.now_ns(), pending = input_pending_ns, cpu0 = 0, t_copy0, t_copy1, t_flip1, t_keep0, t_keep1;
    int any = 0;

    uint64_t slot_ns = 0;

    (void)context;
    if (!bgra || !width || !height || width > W || height > H) return -1;
    /* The guest window's CPU end, before any wait of this present (the first
     * p4 launch read it after the slot wait and counted that as guest CPU). */
    if (bench_plan) cpu0 = thread_cpu_us();
    /* Async: the next back buffer must be neither queued nor on screen.
     * Outside present_lock (presents are one at a time: ntdll's sink lock). */
    if (p_async && video_open) {
        uint64_t w0 = cfg.now_ns();
        pw_videoout_orbis_wait_slot(&video);
        slot_ns = cfg.now_ns() - w0;
    }
    pthread_mutex_lock(&present_lock);
    if (!video_open) {
        gdi_dropped++;
        note_pending = 0;
        pthread_mutex_unlock(&present_lock);
        return -1;
    }
    if (pending) {
        uint64_t lat = t0 - pending;
        input_pending_ns = 0;
        lat_n++;
        lat_sum_ns += lat;
        if (lat > lat_max_ns) lat_max_ns = lat;
    }
    scale = W / width < H / height ? W / width : H / height;
    if (!scale) scale = 1;
    x0 = (W - width * scale) / 2;
    y0 = (H - height * scale) / 2;
    present_scale = scale;
    present_x0 = x0;
    present_y0 = y0;
    t_copy0 = cfg.now_ns();
    dst = pw_videoout_orbis_back(&video);
    if (p_path != P_PATH_SCALE) {
        /* The driver's B,G,R,X is A8R8G8B8's memory order (alpha as the driver
         * left it): rows copied, a scaled row widened once and copied again. */
        const uint32_t bw = video.mode.width, bh = video.mode.height, pitch = video.mode.pitch;
        uint32_t bs = bw / width < bh / height ? bw / width : bh / height;
        if (!bs) bs = 1;
        buf_scale = bs;
        buf_x0 = (bw - (width < bw ? width : bw) * bs) / 2;
        buf_y0 = (bh - (height < bh ? height : bh) * bs) / 2;
        if (cleared[video.back][0] != width || cleared[video.back][1] != height) {
            for (uint32_t y = 0; y < bh; y++)
                for (uint32_t x = 0; x < bw; x++) dst[(size_t)y * pitch + x] = 0xff000000u;
            cleared[video.back][0] = width;
            cleared[video.back][1] = height;
        }
        for (uint32_t y = 0; y < height && y * bs < bh; y++) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)bgra + (size_t)y * stride);
            uint32_t *row = dst + (size_t)(buf_y0 + y * bs) * pitch + buf_x0;
            uint32_t w = width * bs <= bw ? width : bw / bs;
            if (bs == 1) {
                memcpy(row, src, (size_t)w * 4);
                continue;
            }
            /* Widened in cached memory, then written bs times: never read
             * back out of the scan-out buffer (write-combined Garlic memory;
             * reading it made the first vo1080 launch's copy 112 ms, LOG.md
             * 2026-11-01 18:36). */
            for (uint32_t x = 0; x < w; x++)
                for (uint32_t dx = 0; dx < bs; dx++) widened[x * bs + dx] = src[x];
            for (uint32_t dy = 0; dy < bs; dy++) memcpy(row + (size_t)dy * pitch, widened, (size_t)w * bs * 4);
        }
        /* The readback's layout: as the buffer for vo1080; for vo720 the
         * buffer unscaled and centred (the readback cannot know VideoOut's
         * scaling, which only HDMI shows). */
        present_scale = bw == W ? bs : 1;
        present_x0 = (W - width * present_scale) / 2;
        present_y0 = (H - height * present_scale) / 2;
        any = 1;   /* not looked at: that would read every pixel again */
        goto copied;
    }
    /* The border around the desktop: once per buffer and frame size, not every frame (8 MB). */
    if (cleared[video.back][0] != width || cleared[video.back][1] != height) {
        for (size_t i = 0; i < (size_t)W * H; i++) dst[i] = 0xff000000u;
        cleared[video.back][0] = width;
        cleared[video.back][1] = height;
    }
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)bgra + (size_t)y * stride);
        if (scale == 1) {
            uint32_t *row = dst + (size_t)(y0 + y) * W + x0;
            for (uint32_t x = 0; x < width; x++) {
                uint32_t v = src[x];
                any |= (v & 0x00ffffffu) != 0;
                row[x] = (v & 0x0000ff00u) | (v >> 16 & 0xffu) | (v & 0xffu) << 16 | 0xff000000u;
            }
            continue;
        }
        for (uint32_t x = 0; x < width; x++) {
            uint32_t v = src[x], rgba = (v & 0x0000ff00u) | (v >> 16 & 0xffu) | (v & 0xffu) << 16 | 0xff000000u;
            any |= (v & 0x00ffffffu) != 0;
            for (uint32_t dy = 0; dy < scale; dy++)
                for (uint32_t dx = 0; dx < scale; dx++)
                    dst[(size_t)(y0 + y * scale + dy) * W + x0 + x * scale + dx] = rgba;
        }
    }
copied:
    t_copy1 = cfg.now_ns();
    pw_videoout_orbis_flip(&video);
    t_flip1 = cfg.now_ns();
    frames++;
    frame_width = width;
    frame_height = height;
    t_keep0 = t_keep1 = t_flip1;
    if (any) {
        drawn++;
    }
    /* The vo paths move this copy to save time: the readback comes from the scan-out buffer shown last. */
    if (any && p_path == P_PATH_SCALE) {
        /* The source frame, not the 8 MB scan-out buffer: the readback is built from it when saved. */
        if (!last_drawn) last_drawn = malloc((size_t)W * H * 4);
        if (last_drawn)
            for (uint32_t y = 0; y < height; y++)
                memcpy(last_drawn + (size_t)y * width, (const uint8_t *)bgra + (size_t)y * stride, (size_t)width * 4);
        t_keep1 = cfg.now_ns();
    }
    if (bench_on) {
        pthread_t self = pthread_self();
        bench_presents++;
        if (prev_valid) {
            pw_hist_add(&hist_guest, (t0 - prev_exit_ns) / 1000);
            pw_hist_add(&hist_frame, (t0 - prev_enter_ns) / 1000);
            if (!pthread_equal(self, prev_thread)) thread_cpu_other++;
            else if (cpu0 && prev_exit_cpu_us && cpu0 >= prev_exit_cpu_us) pw_hist_add(&hist_guest_cpu, cpu0 - prev_exit_cpu_us);
        }
        pw_hist_add(&hist_copy, (t_copy1 - t_copy0) / 1000);
        pw_hist_add(&hist_flip, (t_flip1 - t_copy1 + slot_ns) / 1000);
        if (any && p_path == P_PATH_SCALE) pw_hist_add(&hist_keep, (t_keep1 - t_keep0) / 1000);
        if (note_pending) {
            bench_noted++;
            if (note_dirty) {
                bench_flushes++;
                bench_dirty_sum += note_dirty;
                if (note_dirty > bench_dirty_max) bench_dirty_max = note_dirty;
                if (note_dirty >= note_screen) bench_full++;
            }
            if (note_composed) bench_composed++;
        }
    }
    note_pending = 0;
    present_n++;
    if (bench_plan) {
        prev_exit_cpu_us = thread_cpu_us();
        prev_thread = pthread_self();
        prev_enter_ns = t0;
        prev_exit_ns = cfg.now_ns();
        prev_valid = 1;
        if (bench_on) pw_hist_add(&hist_present, (prev_exit_ns - t0) / 1000);
        present_sum_ns += prev_exit_ns - t0;
    } else {
        present_sum_ns += cfg.now_ns() - t0;
    }
    pthread_mutex_unlock(&present_lock);
    return 0;
}

/* The last drawn frame to <root>/<name>, laid out as the scan-out buffer
 * (centred, scaled, R,G,B,A) for fbshot.sh; 0 on success. */
static int save_drawn_as(const char *name)
{
    const uint32_t W = PW_VIDEOOUT_ORBIS_WIDTH, H = PW_VIDEOOUT_ORBIS_HEIGHT;
    char shot[512];
    PwVideoOutOrbis copy;
    uint32_t *frame;
    int rc = -1;

    pthread_mutex_lock(&present_lock);
    if (p_path != P_PATH_SCALE && video_open && frame_width && frame_height && buf_scale &&
        video.buffers[video.shown]) {
        /* The keep copy, moved here: the frame shown last, sampled out of the
         * scan-out buffer (write-combined memory, slow to read, once). */
        const uint32_t *shown = video.buffers[video.shown];
        if (!last_drawn) last_drawn = malloc((size_t)W * H * 4);
        if (last_drawn)
            for (uint32_t y = 0; y < frame_height && buf_y0 + y * buf_scale < video.mode.height; y++)
                for (uint32_t x = 0; x < frame_width && buf_x0 + x * buf_scale < video.mode.width; x++)
                    last_drawn[(size_t)y * frame_width + x] =
                        shown[(size_t)(buf_y0 + y * buf_scale) * video.mode.pitch + buf_x0 + x * buf_scale];
    }
    if (last_drawn && present_scale && (frame = malloc((size_t)W * H * 4))) {
        for (size_t i = 0; i < (size_t)W * H; i++) frame[i] = 0xff000000u;
        for (uint32_t y = 0; y < frame_height * present_scale; y++)
            for (uint32_t x = 0; x < frame_width * present_scale; x++) {
                uint32_t v = last_drawn[(size_t)(y / present_scale) * frame_width + x / present_scale];
                frame[(size_t)(present_y0 + y) * W + present_x0 + x] =
                    (v & 0x0000ff00u) | (v >> 16 & 0xffu) | (v & 0xffu) << 16 | 0xff000000u;
            }
        copy = video;
        copy.buffers[copy.shown] = frame;
        copy.mode.width = copy.mode.pitch = W;   /* fbshot.sh reads a 1920x1080 frame */
        copy.mode.height = H;
        snprintf(shot, sizeof(shot), "%s/%s", cfg.root, name);
        rc = pw_videoout_orbis_save(&copy, shot);
        free(frame);
        step("present: last drawn frame of %llu kept as %s (%s)", (unsigned long long)frames, shot,
             rc ? "failed" : "ok");
    }
    pthread_mutex_unlock(&present_lock);
    return rc;
}

/* The last drawn frame to <root>/shot.raw (fbshot.sh's readback); once. */
static void save_last_drawn(void)
{
    /* The vo paths fill last_drawn only here, from the scan-out buffer. */
    if (fb_saved == -1 && (last_drawn || (p_path != P_PATH_SCALE && frame_width)))
        fb_saved = save_drawn_as("shot.raw") ? 1 : 0;
}

/* ---- d4: winemine and a scripted click ------------------------------------------- */

/* The click and the cell it opens, in desktop coordinates, from the host
 * reference run (tools/test_wine_ps5_driver.sh --click 80,153 winemine):
 * winemine's window at 0,0 of the 800x600 desktop, its 9x9 board's cells
 * 16 px from 8,81; the middle cell (4,4) spans 72..87 x 145..160, and the
 * first click is always safe, so it opens: its pixel 74,150 goes from the
 * closed cell's 0,128,0 to the open cell's 0,0,0, clear of the digit and of
 * the pointer drawn down and right of 80,153. */
enum { D4_CLICK_X = 80, D4_CLICK_Y = 153, D4_PROBE_X = 74, D4_PROBE_Y = 150 };

static int (*post_input)(const void *event);
static volatile int d4_board_opened = -1, d4_refused, d4_posted;
static uint32_t d4_before, d4_after;

void pw_ms_set_input(int (*post)(const void *event)) { post_input = post; }

/* A desktop pixel of the last drawn frame as 0xRRGGBB, or 0xffffffff. */
static uint32_t desktop_pixel(uint32_t x, uint32_t y)
{
    uint32_t v = 0xffffffffu;
    pthread_mutex_lock(&present_lock);
    if (last_drawn && x < frame_width && y < frame_height)
        v = last_drawn[(size_t)y * frame_width + x] & 0x00ffffffu;   /* BGRA in memory: 0xRRGGBB as a word */
    pthread_mutex_unlock(&present_lock);
    return v;
}

static void pause_ms(unsigned ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
    nanosleep(&t, NULL);
}

static void post(uint32_t type, uint32_t code, int32_t x, int32_t y, uint32_t down)
{
    PwWineInput event = { type, code, x, y, down };
    if (!post_input || post_input(&event)) d4_refused++;
    else d4_posted++;
}

static void *save_readback(void *arg)
{
    (void)arg;
    save_last_drawn();
    return NULL;
}

/* Waits for winemine's first frame, clicks the middle cell as the host run
 * did (relative motion since patch 0670: to the corner, then by the point),
 * waits for the cell to change and keeps that frame as the readback: after
 * settle_ms on this thread (d4), or at once on another (d: the pad loop
 * starts without waiting; the user found the stick dead for the first
 * seconds while this thread settled and wrote 8 MB). */
static void d4_click(unsigned settle_ms)
{
    uint64_t until;
    for (until = cfg.now_ns() + 90000000000ull; cfg.now_ns() < until && (!post_input || !drawn);) pause_ms(100);
    pause_ms(3000);
    d4_before = desktop_pixel(D4_PROBE_X, D4_PROBE_Y);
    step("d4 before click: frames=%llu drawn=%llu probe=%06x post_input=%d", (unsigned long long)frames,
         (unsigned long long)drawn, (unsigned)d4_before, post_input != NULL);
    post(2, 0, -100000, -100000, 0);   /* PW_WINE_INPUT_MOUSE_MOVE */
    pause_ms(50);
    post(2, 0, D4_CLICK_X, D4_CLICK_Y, 0);
    pause_ms(50);
    post(3, 0, 0, 0, 1);               /* PW_WINE_INPUT_MOUSE_BUTTON, left */
    pause_ms(50);
    post(3, 0, 0, 0, 0);
    pause_ms(50);
    for (until = cfg.now_ns() + 10000000000ull; cfg.now_ns() < until; pause_ms(100))
        if ((d4_after = desktop_pixel(D4_PROBE_X, D4_PROBE_Y)) != d4_before) break;
    d4_board_opened = d4_after != d4_before && d4_after != 0xffffffffu;
    step("d4 after click: frames=%llu probe=%06x board_opened=%d", (unsigned long long)frames, (unsigned)d4_after,
         d4_board_opened);
    if (settle_ms) {
        pause_ms(settle_ms);
        save_last_drawn();
    } else {
        pthread_t thread;
        if (pthread_create(&thread, NULL, save_readback, NULL)) save_last_drawn();
        else pthread_detach(thread);
    }
}

/* d4: the click, then the Wine stage ends here: winemine does not exit. */
static void *d4_script(void *arg)
{
    (void)arg;
    d4_click(2000);
    pw_ms_wine_end("running", "", -1);
    return NULL;
}

/* ---- d: the DualShock 4 drives the pointer ------------------------------------------ */

/* After d4's click (board_opened, the readback), the pad's left stick moves
 * the pointer (relative motion, pw_wine_pointer_step: dead zone and squared
 * curve, at most D_POINTER_SPEED px/s), cross is the left button and circle
 * the right, until OPTIONS is pressed or D_PLAY_S seconds pass (the Wine
 * stage's watchdog is 180 s). The last frame is kept as shot-end.raw. */
enum { D_POINTER_SPEED = 600, D_PLAY_S = 150 };

static volatile int d_inputs, d_pad = -1, d_pad_reads;
static const char *d_end = "-";

static void *d_script(void *arg)
{
    static const struct { uint32_t mask; uint32_t button; } buttons[] = {
        { PW_PAD_ORBIS_CROSS, 0 }, { PW_PAD_ORBIS_CIRCLE, 1 },
    };
    PwPadOrbis pad;
    PwPadOrbisState state;
    PwWinePointer pointer = { 0, 0 };
    uint32_t held = 0;
    uint64_t last, until, started, next_report;
    (void)arg;

    d4_click(0);
    if ((d_pad = pw_pad_orbis_open(&pad) == 0)) {
        step("d pad open user=0x%x handle=%d: stick moves the pointer, cross/circle click, OPTIONS ends",
             (unsigned)pad.user, pad.handle);
        last = started = next_report = cfg.now_ns();
        for (until = last + (uint64_t)D_PLAY_S * 1000000000ull; cfg.now_ns() < until; pause_ms(8)) {
            uint64_t now = cfg.now_ns();
            PwWineInput move;
            memset(&state, 0, sizeof(state));
            if (pw_pad_orbis_read(&pad, &state)) continue;
            d_pad_reads++;
            /* A record a second: a session closed from the PS menu ends no stage. */
            if (now >= next_report) {
                step("d play t=%llus reads=%d inputs=%d refused=%d frames=%llu buttons=0x%x stick=%u,%u "
                     "lat_avg_ms=%llu lat_max_ms=%llu present_ms=%.1f",
                     (unsigned long long)((now - started) / 1000000000ull), d_pad_reads, d_inputs, d4_refused,
                     (unsigned long long)frames, (unsigned)state.buttons, state.left_x, state.left_y,
                     (unsigned long long)(lat_n ? lat_sum_ns / lat_n / 1000000ull : 0),
                     (unsigned long long)(lat_max_ns / 1000000ull),
                     present_n ? present_sum_ns / (double)present_n / 1e6 : 0.0);
                next_report = now + 1000000000ull;
            }
            if (state.buttons & PW_PAD_ORBIS_OPTIONS) { d_end = "options"; break; }
            if (pw_wine_pointer_step(&pointer, state.left_x, state.left_y, D_POINTER_SPEED,
                                     (uint32_t)((now - last) / 1000), &move)) {
                if (!input_pending_ns) input_pending_ns = now;
                post(move.type, move.code, move.x, move.y, 0);
                d_inputs++;
            }
            last = now;
            for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
                uint32_t down = state.buttons & buttons[i].mask;
                if (down != (held & buttons[i].mask)) {
                    if (!input_pending_ns) input_pending_ns = now;
                    post(3, buttons[i].button, 0, 0, down != 0);   /* PW_WINE_INPUT_MOUSE_BUTTON */
                    d_inputs++;
                }
            }
            held = state.buttons;
        }
        if (!strcmp(d_end, "-")) d_end = "time";
    } else {
        step("d pad open failed: %s 0x%x", pad.failed, (unsigned)pad.status);
        d_end = "nopad";
    }
    save_drawn_as("shot-end.raw");
    pw_ms_wine_end("running", "", -1);
    return NULL;
}

/* ---- f: Space Cadet Pinball with the DualShock 4 ------------------------------------- */

/* prospero-win's pinball bindings (docs/CASE_STUDY_PINBALL.md) in its .input
 * format (docs/CONTROLS.md); <root>/input/pinball.input replaces them. L1/R1
 * are the flippers, cross the plunger, the d-pad's left/right/up nudge,
 * options pauses (F3), square starts a new game (F2), circle answers a
 * dialog (Enter); the left stick moves the pointer, R2/L2 click. Holding
 * the touchpad for F_END_HOLD_S ends the session and sends the line, as do
 * F_PLAY_S seconds; Pinball goes on running after it. */
static const char f_preset[] =
    "[input]\n"
    "mode = keyboard\n"
    "mouse = left_stick\n"
    "l1 = z\nr1 = slash\ncross = space\n"
    "left = x\nright = period\nup = up\n"
    "options = f3\nsquare = f2\ncircle = enter\n"
    "r2 = mouse_left\nl2 = mouse_right\n";
enum { F_PLAY_S = 1800, F_END_HOLD_S = 2, F_TOUCHPAD = 0x100000u };
static volatile int f_keys;
static const char *f_preset_from = "-";
static uint64_t f_play_ns, f_frames_play;

static int f_bindings(PwGameInput *input)
{
    static uint8_t text[4096];
    char path[512];
    FILE *file;

    pw_game_input_init(input);
    snprintf(path, sizeof(path), "%s/input/pinball.input", cfg.root);
    if ((file = fopen(path, "rb"))) {
        size_t got = fread(text, 1, sizeof(text), file);
        fclose(file);
        if (pw_game_input_parse(text, got, input) == PW_OK) {
            f_preset_from = "file";
            return 0;
        }
        step("f %s does not parse: the built-in bindings", path);
        pw_game_input_init(input);
    }
    f_preset_from = "builtin";
    return pw_game_input_parse((const uint8_t *)f_preset, sizeof(f_preset) - 1, input) == PW_OK ? 0 : -1;
}

/* d's loop with key bindings on an open pad: each binding's button edge
 * becomes a key or a mouse button of the user driver's input queue; a
 * record a second; until the touchpad is held or F_PLAY_S pass. Sets
 * d_end, f_play_ns and f_frames_play. */
static void f_play(const PwPadOrbis *pad_in)
{
    PwGameInput input;
    PwPadOrbis pad = *pad_in;
    PwPadOrbisState state;
    PwWinePointer pointer = { 0, 0 };
    uint32_t held = 0;
    uint64_t last, started = cfg.now_ns(), until, next_report, touch_since = 0, frames0;

    if (f_bindings(&input)) step("f: the built-in bindings do not parse");
    pthread_mutex_lock(&present_lock);
    frames0 = frames;
    pthread_mutex_unlock(&present_lock);
    {
        step("f pad open user=0x%x handle=%d preset=%s: L1/R1 flippers, cross plunger, d-pad nudges, square new "
             "game, options pause, circle enter; hold the touchpad %d s to end", (unsigned)pad.user, pad.handle,
             f_preset_from, F_END_HOLD_S);
        last = started = next_report = cfg.now_ns();
        for (until = started + (uint64_t)F_PLAY_S * 1000000000ull; cfg.now_ns() < until; pause_ms(8)) {
            uint64_t now = cfg.now_ns();
            PwWineInput move;
            memset(&state, 0, sizeof(state));
            if (pw_pad_orbis_read(&pad, &state)) continue;
            d_pad_reads++;
            if (now >= next_report) {
                step("f play t=%llus reads=%d keys=%d inputs=%d refused=%d frames=%llu buttons=0x%x present_ms=%.1f",
                     (unsigned long long)((now - started) / 1000000000ull), d_pad_reads, f_keys, d_inputs, d4_refused,
                     (unsigned long long)frames, (unsigned)state.buttons,
                     present_n ? present_sum_ns / (double)present_n / 1e6 : 0.0);
                next_report = now + 1000000000ull;
            }
            if (state.buttons & F_TOUCHPAD) {
                if (!touch_since) touch_since = now;
                else if (now - touch_since >= (uint64_t)F_END_HOLD_S * 1000000000ull) { d_end = "touchpad"; break; }
            } else {
                touch_since = 0;
            }
            if (input.mouse == PW_GAME_STICK_LEFT &&
                pw_wine_pointer_step(&pointer, state.left_x, state.left_y, D_POINTER_SPEED,
                                     (uint32_t)((now - last) / 1000), &move)) {
                if (!input_pending_ns) input_pending_ns = now;
                post(move.type, move.code, move.x, move.y, 0);
                d_inputs++;
            }
            last = now;
            for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++) {
                const PwGameBinding *bind = &input.bindings[i];
                uint32_t mask = pw_game_button_mask(i), down = state.buttons & mask;
                if (!mask || down == (held & mask)) continue;
                if (bind->kind == PW_GAME_BIND_KEY) {
                    post(1, bind->code, 0, 0, down != 0);   /* PW_WINE_INPUT_KEY: a Windows virtual key */
                    f_keys++;
                } else if (bind->kind == PW_GAME_BIND_MOUSE) {
                    post(3, bind->code, 0, 0, down != 0);   /* PW_WINE_INPUT_MOUSE_BUTTON */
                } else {
                    continue;
                }
                if (!input_pending_ns) input_pending_ns = now;
                d_inputs++;
            }
            held = state.buttons;
        }
        if (!strcmp(d_end, "-")) d_end = "time";
        /* No key stays down after the session. */
        for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++)
            if ((held & pw_game_button_mask(i)) && input.bindings[i].kind == PW_GAME_BIND_KEY)
                post(1, input.bindings[i].code, 0, 0, 0);
    }
    f_play_ns = cfg.now_ns() - started;
    pthread_mutex_lock(&present_lock);
    f_frames_play = frames - frames0;
    pthread_mutex_unlock(&present_lock);
}

static void *f_script(void *arg)
{
    PwPadOrbis pad;
    (void)arg;

    if ((d_pad = pw_pad_orbis_open(&pad) == 0)) {
        f_play(&pad);
    } else {
        step("f pad open failed: %s 0x%x", pad.failed, (unsigned)pad.status);
        d_end = "nopad";
    }
    pw_ms_wine_end("running", "", -1);
    return NULL;
}

/* ---- phase p2: native WoW64 readiness, measured by the title before Wine ------------- */

/* What patch 0883 (PS5) relies on, measured on this PS4 before an Orbis
 * variant of it is written (docs/ps4/HANDOFF-p.md, p2): libkernel's getppid
 * stub has the shape "mov $N,%rax; mov %rcx,%r10; syscall; jb; ret", checked
 * by its bytes; its syscall instruction runs other calls (getpid and getppid
 * swapped); sigaltstack as system call 53 through it (the libkernel export
 * returned -1 to Wine in c and d); a signal registered with the kernel's own
 * sigaction (416) and SA_ONSTACK arrives on that stack, where 0883's entry
 * finds its thread by rounding the stack pointer down to 64 KiB; and the
 * frame holds a marked MXCSR at ucontext+320+24 and the FS base at +1152.
 * FreeBSD's numbers and layouts throughout: the SDK's musl headers number
 * signals differently. Everything is undone before Wine starts. */
enum {
    K_SYS_GETPID = 20, K_SYS_GETPPID = 39, K_SYS_SIGALTSTACK = 53, K_SYS_SIGACTION = 416,
    K_SIGUSR2 = 31, K_SA_ONSTACK = 0x1, K_SA_SIGINFO = 0x40, K_SS_DISABLE = 0x4,
    K_SIG_UNBLOCK = 2, K_SIG_SETMASK = 3,   /* FreeBSD's; the SDK's signal.h has musl's 1 and 2 */
    K_ALT_ALIGN = 0x10000, K_ALT_MARK = 0x5057414c54535431ull & 0xffffffff,
};
typedef struct KStack { void *sp; size_t size; int flags; } KStack;                   /* FreeBSD stack_t */
typedef struct KSigaction { uintptr_t handler; int flags; uint32_t mask[4]; } KSigaction;   /* the kernel's */

/* Runs system call NUMBER through the syscall instruction at ENTRY, inside
 * libkernel; the stub's own error path follows it (-1, errno set). From
 * patch 0883's pw_native_syscall_at (this repository, LGPL). */
extern long pw_ms_syscall_at(uintptr_t entry, long number, long a1, long a2, long a3);
__asm__(".text\n.globl pw_ms_syscall_at\n.type pw_ms_syscall_at,@function\npw_ms_syscall_at:\n"
        "movq %rsi,%rax\n movq %rdi,%r11\n movq %rdx,%rdi\n movq %rcx,%rsi\n movq %r8,%rdx\n"
        "subq $8,%rsp\n callq *%r11\n addq $8,%rsp\n ret\n.size pw_ms_syscall_at,.-pw_ms_syscall_at\n");

static volatile int probe_hits;
static volatile uintptr_t probe_rsp, probe_found;
static volatile uint32_t probe_mxcsr;
static volatile uint64_t probe_fsbase;

static void probe_handler(int signal, siginfo_t *info, void *context)
{
    uintptr_t rsp = (uintptr_t)__builtin_frame_address(0);
    (void)signal;
    (void)info;
    probe_rsp = rsp;
    /* 0883's lookup: the 64 KiB-aligned base of the signal stack holds the thread's data. */
    probe_found = *(volatile uint64_t *)(rsp & ~(uintptr_t)(K_ALT_ALIGN - 1));
    memcpy((void *)&probe_mxcsr, (const uint8_t *)context + 320 + 24, 4);
    memcpy((void *)&probe_fsbase, (const uint8_t *)context + 1152, 8);
    probe_hits++;
}

extern int sysarch(int number, void *args);

static void stage_native_probe(char *fields, size_t size)
{
    const uint8_t *stub = (const uint8_t *)(uintptr_t)&getppid;
    uintptr_t entry = 0, base;
    uint8_t *region;
    KStack ss, cur, lib;
    KSigaction act, old;
    uint32_t unblock[4] = { 1u << (K_SIGUSR2 - 1), 0, 0, 0 }, saved[4] = { 0, 0, 0, 0 };   /* FreeBSD sigset_t */
    uint64_t fsbase = 0;
    uint32_t mxcsr, marked;
    long pid = getpid(), ppid = getppid(), rc;
    int caps = 0, lib_rc, lib_errno = 0, alt_ok = 0, act_ok = 0, onstack = 0, lookup = 0, frame = 0;
    char stub_bytes[40];
    size_t n = 0;

    /* 1. The stub's shape, by its bytes (milestone a read them at getppid). */
    snprintf(stub_bytes, sizeof(stub_bytes), "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             stub[0], stub[1], stub[2], stub[3], stub[4], stub[5], stub[6], stub[7], stub[8], stub[9], stub[10],
             stub[11], stub[12], stub[13], stub[14]);
    if (stub[0] == 0x48 && stub[1] == 0xc7 && stub[2] == 0xc0 && stub[7] == 0x49 && stub[8] == 0x89 &&
        stub[9] == 0xca && stub[10] == 0x0f && stub[11] == 0x05)
        entry = (uintptr_t)stub + 10;
    n += (size_t)snprintf(fields + n, size - n, "stub=%s stub_bytes=%s", entry ? "ok" : "shape", stub_bytes);
    step("p2 getppid at %p: %s (%s)", (const void *)stub, stub_bytes, entry ? "the stub shape" : "not the stub shape");
    if (!entry) goto done;

    /* 2. The instruction runs whichever call %rax names. */
    if (pid <= 0 || pid == ppid || pw_ms_syscall_at(entry, K_SYS_GETPID, 0, 0, 0) != pid ||
        pw_ms_syscall_at(entry, K_SYS_GETPPID, 0, 0, 0) != ppid) {
        n += (size_t)snprintf(fields + n, size - n, " raw_syscall=fail");
        goto done;
    }
    n += (size_t)snprintf(fields + n, size - n, " raw_syscall=ok");
    caps |= 1;

    /* 3. A 64 KiB-aligned stack with a marker at its base, as Wine's signal stack has its thread data. */
    region = malloc(2 * K_ALT_ALIGN);
    if (!region) {
        n += (size_t)snprintf(fields + n, size - n, " altstack53=nomemory");
        goto done;
    }
    base = ((uintptr_t)region + K_ALT_ALIGN - 1) & ~(uintptr_t)(K_ALT_ALIGN - 1);
    *(volatile uint64_t *)base = K_ALT_MARK;
    ss.sp = (void *)(base + 4096);
    ss.size = K_ALT_ALIGN - 4096;
    ss.flags = 0;

    /* libkernel's sigaltstack (Wine's init_thread_pipe calls it), for comparison, then off again. */
    errno = 0;
    lib = ss;
    lib_rc = sigaltstack((const stack_t *)(void *)&lib, NULL);
    lib_errno = errno;
    if (!lib_rc) {
        KStack off = { NULL, 0, K_SS_DISABLE };
        sigaltstack((const stack_t *)(void *)&off, NULL);
    }
    n += (size_t)snprintf(fields + n, size - n, " altstack_lib=%s%d", lib_rc ? "err:" : "ok", lib_rc ? lib_errno : 0);
    if (!lib_rc) n -= 1;   /* "ok0" -> "ok" */
    fields[n] = 0;

    /* 4. System call 53 through the stub, and read back. */
    errno = 0;
    rc = pw_ms_syscall_at(entry, K_SYS_SIGALTSTACK, (long)(uintptr_t)&ss, 0, 0);
    if (rc) {
        n += (size_t)snprintf(fields + n, size - n, " altstack53=err:%d", errno);
        goto release;
    }
    memset(&cur, 0, sizeof(cur));
    pw_ms_syscall_at(entry, K_SYS_SIGALTSTACK, 0, (long)(uintptr_t)&cur, 0);
    alt_ok = cur.sp == ss.sp && cur.size == ss.size && !(cur.flags & K_SS_DISABLE);
    n += (size_t)snprintf(fields + n, size - n, " altstack53=%s", alt_ok ? "ok" : "readback");

    /* 5. The kernel's own sigaction with SA_ONSTACK, one SIGUSR2 with a marked MXCSR. */
    memset(&act, 0, sizeof(act));
    memset(&old, 0, sizeof(old));
    act.handler = (uintptr_t)probe_handler;
    act.flags = K_SA_SIGINFO | K_SA_ONSTACK;
    errno = 0;
    if (pw_ms_syscall_at(entry, K_SYS_SIGACTION, K_SIGUSR2, (long)(uintptr_t)&act, (long)(uintptr_t)&old)) {
        n += (size_t)snprintf(fields + n, size - n, " raw_sigaction=err:%d", errno);
        goto disable;
    }
    act_ok = 1;
    sysarch(128 /* AMD64_GET_FSBASE */, &fsbase);
    pthread_sigmask(K_SIG_UNBLOCK, (const sigset_t *)(void *)unblock, (sigset_t *)(void *)saved);
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    marked = (mxcsr & ~0x6000u) | 0x2000u;   /* round down: visible and harmless */
    __asm__ volatile("ldmxcsr %0" ::"m"(marked));
    pthread_kill(pthread_self(), K_SIGUSR2);
    __asm__ volatile("ldmxcsr %0" ::"m"(mxcsr));
    pthread_sigmask(K_SIG_SETMASK, (const sigset_t *)(void *)saved, NULL);
    pw_ms_syscall_at(entry, K_SYS_SIGACTION, K_SIGUSR2, (long)(uintptr_t)&old, 0);
    onstack = probe_hits == 1 && probe_rsp >= (uintptr_t)ss.sp && probe_rsp < (uintptr_t)ss.sp + ss.size;
    lookup = probe_hits == 1 && probe_found == K_ALT_MARK;
    frame = probe_hits == 1 && probe_mxcsr == marked && fsbase && probe_fsbase == fsbase;
    n += (size_t)snprintf(fields + n, size - n, " raw_sigaction=ok hits=%d onstack=%d lookup=%d frame_check=%s "
                          "mxcsr=%x/%x fsbase=%llx/%llx", probe_hits, onstack, lookup, frame ? "ok" : "bad",
                          (unsigned)probe_mxcsr, (unsigned)marked, (unsigned long long)probe_fsbase,
                          (unsigned long long)fsbase);
    if (alt_ok && onstack && lookup) caps |= 2;
    if (frame) caps |= 4;
disable:
    {
        KStack off = { NULL, 0, K_SS_DISABLE };
        pw_ms_syscall_at(entry, K_SYS_SIGALTSTACK, (long)(uintptr_t)&off, 0, 0);
    }
release:
    /* The region stays allocated: a late signal on it must not land in freed memory. */
    (void)act_ok;
done:
    snprintf(fields + n, size - n, " probe_caps=%d", caps);
    step("p2 native probe: %s", fields);
}

/* ---- phase p5: the title's audio port, the sink of Wine's audio driver ---------------- */

/* One sceAudioOut port for the process, opened once and never closed (a
 * process gets one output stream; close and reopen is disputed:
 * orbis-porting-kit services/audio/orbis_audio.h), 48 kHz S16 stereo in
 * 256-frame grains, the volume set after Open (the port can open silent).
 * The SYSTEM user 0xFF: milestone a's probe opened with the initial user and
 * got 0x809b0001 (klog: sceMbusAddHandleByUserId); every reference opens
 * with 0xFF. The initial user is tried only if 0xFF fails, for the record.
 * sceAudioOutOutput blocks until the port has room: the port is the clock,
 * and the sink runs on the driver's mixer thread (wine/wineps5). */
extern int32_t sceAudioOutInit(void);
extern int32_t sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t rate,
                               uint32_t format);
extern int32_t sceAudioOutSetVolume(int32_t handle, int32_t flags, const int32_t *volumes);
extern int32_t sceAudioOutOutput(int32_t handle, const void *frames);
extern int32_t sceUserServiceInitialize(const void *parameters);
extern int32_t sceUserServiceGetInitialUser(int32_t *user);
enum { AUDIO_USER_SYSTEM = 0xff, AUDIO_PORT_MAIN = 0, AUDIO_GRAIN = 256, AUDIO_RATE = 48000,
       AUDIO_S16_STEREO = 1, AUDIO_VOLUME_FL_FR = 3, AUDIO_VOLUME_0DB = 0x8000,
       AUDIO_GRAIN_NS = AUDIO_GRAIN * 1000000000ull / AUDIO_RATE };
static int p_audio = -1;              /* PS4WINE_AUDIO: 1 on, 0 off (p5: on) */
static int32_t audio_handle = -1;
static volatile uint64_t audio_grains, audio_nonzero, audio_errors, audio_gaps, audio_gap_max_ns;
static uint64_t audio_last_ns;

/* gaps: a pause between two grains longer than two grains and shorter than
 * a second. It is not an underrun: the mixer also waits whenever no stream
 * is started, between two sounds (p5's first count, 13 at 2026-11-01 21:00,
 * mixed both). The underruns, silence inside a sound, are the driver's
 * (pw_wineps5_stats). */
int pw_ms_audio(void *context, const int16_t *frames)
{
    uint64_t now = cfg.now_ns();
    int nonzero = 0, rc;

    (void)context;
    if (audio_handle < 0) return -1;
    if (audio_last_ns) {
        uint64_t gap = now - audio_last_ns;
        if (gap > 2 * AUDIO_GRAIN_NS && gap < 1000000000ull) audio_gaps++;
        if (gap < 1000000000ull && gap > audio_gap_max_ns) audio_gap_max_ns = gap;
    }
    for (unsigned i = 0; i < 2 * AUDIO_GRAIN && !nonzero; i++) nonzero = frames[i] != 0;
    rc = sceAudioOutOutput(audio_handle, frames);
    audio_last_ns = cfg.now_ns();
    if (rc < 0) {
        audio_errors++;
        return -1;
    }
    audio_grains++;
    audio_nonzero += nonzero;
    return 0;
}

int pw_ms_audio_wanted(void) { return p_audio == 1 && audio_handle >= 0; }

static void stage_audio(char *fields, size_t size)
{
    int32_t volumes[8], user = -1, rc_init, rc_vol;
    int32_t h;

    if (p_audio != 1) {
        snprintf(fields, size, "audio=off");
        return;
    }
    rc_init = sceAudioOutInit();
    h = sceAudioOutOpen(AUDIO_USER_SYSTEM, AUDIO_PORT_MAIN, 0, AUDIO_GRAIN, AUDIO_RATE, AUDIO_S16_STEREO);
    if (h < 0) {
        int32_t h2;
        sceUserServiceInitialize(NULL);
        sceUserServiceGetInitialUser(&user);
        h2 = sceAudioOutOpen(user, AUDIO_PORT_MAIN, 0, AUDIO_GRAIN, AUDIO_RATE, AUDIO_S16_STEREO);
        snprintf(fields, size, "audio=on audio_open=err:0x%x audio_user=system audio_init=0x%x "
                 "audio_initial_user=%s0x%x", (unsigned)h, (unsigned)rc_init, h2 < 0 ? "err:" : "ok:",
                 (unsigned)(h2 < 0 ? h2 : user));
        if (h2 >= 0) audio_handle = h2;
        return;
    }
    for (int i = 0; i < 8; i++) volumes[i] = AUDIO_VOLUME_0DB;
    rc_vol = sceAudioOutSetVolume(h, AUDIO_VOLUME_FL_FR, volumes);
    audio_handle = h;
    step("p5 audio port open handle=0x%x (SYSTEM user 0xff) init=0x%x volume=0x%x", (unsigned)h,
         (unsigned)rc_init, (unsigned)rc_vol);
    snprintf(fields, size, "audio=on audio_open=ok audio_user=system audio_handle=0x%x audio_init=0x%x "
             "audio_volume=0x%x", (unsigned)h, (unsigned)rc_init, (unsigned)rc_vol);
}

/* After the segment: keys that make Pinball play its WAV effects (F2 a new
 * game, the plunger held 1 s, the flippers), posted by the title; the pad
 * is still off. About 12 s. */
static int p_tail_keys;
static double p_tail_s;
static uint64_t p_tail_nonzero0;

static void tail_key(uint32_t vk, unsigned hold_ms, unsigned after_ms)
{
    post(1, vk, 0, 0, 1);
    pause_ms(hold_ms);
    post(1, vk, 0, 0, 0);
    p_tail_keys++;
    pause_ms(after_ms);
}

static void p5_tail(void)
{
    uint64_t t0 = cfg.now_ns();
    p_tail_nonzero0 = audio_nonzero;
    step("p5 tail: F2, the plunger, the flippers (title-injected keys, the pad unused)");
    tail_key(0x71 /* F2 */, 100, 2000);
    tail_key(0x20 /* space: the plunger */, 1000, 2500);
    for (int i = 0; i < 3; i++) {
        tail_key(0x5a /* Z: left flipper */, 200, 500);
        tail_key(0xbf /* '/': right flipper */, 200, 500);
    }
    pause_ms(2500);
    p_tail_s = (cfg.now_ns() - t0) / 1e9;
}

/* ---- phase p: the benchmark segment ------------------------------------------------ */

/* Pinball as plan f has it, but no pad bindings until the segment is over:
 * P_WARM_S from the first present (the first frame the driver sends after
 * Wine starts), then P_SEG_S of attract mode with the measurements on
 * (bench_on). The pad is read throughout and every event counted, not
 * delivered: a touched pad shows as inputs>0 and fails the run instead of
 * skewing it. The line goes out at the end of the segment; then the
 * bindings come on and the user may play (f_play), recorded as a PW_MS play
 * record, never compared. The same W and N every run. */
enum { P_WARM_S = 20, P_SEG_S = 60, P_FIRST_WAIT_S = 600, P_STICK_DEAD = 40, P_TRIGGER_DEAD = 64 };
static int p_inputs, p_inputs_warm, p_pad_reads;
static const char *p_end = "-";
static double p_first_s = -1.0, p_seg_s;
static char p_bench[1700];   /* p_bench_fields, for the p line */
static PwHist p_snap[7];
static int shown_rc0 = -1;
static int64_t p_snap_shown = -1;           /* flips the display completed in the segment; -1 unknown */
static uint64_t p_snap_slot_waits;
static uint64_t p_snap_presents, p_snap_noted, p_snap_flushes, p_snap_full, p_snap_composed, p_snap_dirty_sum,
    p_snap_dirty_max, p_snap_cpu_other;

/* Pad events since the previous state: button edges, and a stick or a
 * trigger leaving its dead zone. */
static int pad_events(const PwPadOrbisState *now, const PwPadOrbisState *before)
{
    int n = __builtin_popcount(now->buttons ^ before->buttons);
    const uint8_t a[4] = { now->left_x, now->left_y, now->right_x, now->right_y };
    const uint8_t b[4] = { before->left_x, before->left_y, before->right_x, before->right_y };
    for (int i = 0; i < 4; i++) {
        int out_now = abs((int)a[i] - 128) > P_STICK_DEAD, out_before = abs((int)b[i] - 128) > P_STICK_DEAD;
        n += out_now && !out_before;
    }
    n += (now->l2 > P_TRIGGER_DEAD) && !(before->l2 > P_TRIGGER_DEAD);
    n += (now->r2 > P_TRIGGER_DEAD) && !(before->r2 > P_TRIGGER_DEAD);
    return n;
}

/* Reads the pad until the time given, counting events into *count. */
static void pad_watch(const PwPadOrbis *pad, int have_pad, PwPadOrbisState *last, uint64_t until, int *count,
                      const char *what, uint64_t since)
{
    uint64_t next_report = cfg.now_ns();
    for (; cfg.now_ns() < until; pause_ms(8)) {
        PwPadOrbisState state;
        uint64_t now = cfg.now_ns();
        if (have_pad) {
            memset(&state, 0, sizeof(state));
            if (!pw_pad_orbis_read(pad, &state)) {
                p_pad_reads++;
                *count += pad_events(&state, last);
                *last = state;
            }
        }
        if (now >= next_report) {
            pthread_mutex_lock(&present_lock);
            step("p %s t=%llus presents=%llu inputs=%d", what, (unsigned long long)((now - since) / 1000000000ull),
                 (unsigned long long)bench_presents, *count);
            pthread_mutex_unlock(&present_lock);
            next_report = now + 10000000000ull;
        }
    }
}

static void *p_script(void *arg)
{
    PwPadOrbis pad;
    PwPadOrbisState last;
    uint64_t begun = cfg.now_ns(), first, seg0, frames_before;
    int have_pad;
    (void)arg;

    /* The pattern stage presented already: the first present is the first after it. */
    pthread_mutex_lock(&present_lock);
    frames_before = frames;
    pthread_mutex_unlock(&present_lock);
    memset(&last, 0, sizeof(last));
    last.left_x = last.left_y = last.right_x = last.right_y = 128;
    have_pad = (d_pad = pw_pad_orbis_open(&pad) == 0);
    if (!have_pad) step("p pad open failed: %s 0x%x (inputs cannot be counted)", pad.failed, (unsigned)pad.status);
    else if (!pw_pad_orbis_read(&pad, &last)) p_pad_reads++;
    for (;;) {
        uint64_t n;
        pthread_mutex_lock(&present_lock);
        n = frames;
        pthread_mutex_unlock(&present_lock);
        if (n > frames_before) break;
        if (cfg.now_ns() - begun > (uint64_t)P_FIRST_WAIT_S * 1000000000ull) {
            p_end = "nopresent";
            pw_ms_wine_end("running", "", -1);
            return NULL;
        }
        pause_ms(50);
    }
    first = cfg.now_ns();
    p_first_s = (first - begun) / 1e9;
    step("p first present %.1f s after the Wine stage began: %d s warm-up, then the %d s segment; the pad is not "
         "used until the line", p_first_s, P_WARM_S, P_SEG_S);
    pad_watch(&pad, have_pad, &last, first + (uint64_t)P_WARM_S * 1000000000ull, &p_inputs_warm, "warm", first);
    pthread_mutex_lock(&present_lock);
    bench_reset();
    bench_on = 1;
    seg0 = cfg.now_ns();
    shown_rc0 = pw_videoout_orbis_shown(&video, &shown_arg0);
    slot_waits0 = video.slot_waits;
    pthread_mutex_unlock(&present_lock);
    pad_watch(&pad, have_pad, &last, seg0 + (uint64_t)P_SEG_S * 1000000000ull, &p_inputs, "segment", seg0);
    pthread_mutex_lock(&present_lock);
    bench_on = 0;
    p_seg_s = (cfg.now_ns() - seg0) / 1e9;
    p_snap[0] = hist_guest; p_snap[1] = hist_copy; p_snap[2] = hist_keep; p_snap[3] = hist_flip;
    p_snap[4] = hist_frame; p_snap[5] = hist_present; p_snap[6] = hist_guest_cpu;
    p_snap_presents = bench_presents; p_snap_noted = bench_noted; p_snap_flushes = bench_flushes;
    p_snap_full = bench_full; p_snap_composed = bench_composed; p_snap_dirty_sum = bench_dirty_sum;
    p_snap_dirty_max = bench_dirty_max; p_snap_cpu_other = thread_cpu_other;
    {
        int64_t arg1 = 0;
        int rc1 = pw_videoout_orbis_shown(&video, &arg1);
        p_snap_shown = !shown_rc0 && !rc1 && arg1 >= shown_arg0 ? (int64_t)(arg1 - shown_arg0) : -1;
        p_snap_slot_waits = video.slot_waits - slot_waits0;
    }
    pthread_mutex_unlock(&present_lock);
    p_end = "segment";
    if (plan->lines == p5_lines) p5_tail();
    /* The histograms for plots, in records of bounded size. */
    {
        static const char *const names[7] = { "guest", "copy", "keep", "flip", "frame", "present", "guest_cpu" };
        char buf[300];
        for (int i = 0; i < 7; i++)
            for (unsigned next = 0, part = 0; next <= PW_HIST_BUCKETS; part++) {
                if (!pw_hist_sparse(&p_snap[i], &next, buf, sizeof(buf))) break;
                step("PW_MS hist %s part=%u bucket_us=%d %s", names[i], part, PW_HIST_BUCKET_US, buf);
            }
    }
    pw_ms_wine_end("running", "", -1);
    if (have_pad) {
        f_play(&pad);
        step("PW_MS play end=%s play_s=%.0f frames=%llu fps=%.1f keys=%d inputs=%d (hand play, not compared)", d_end,
             f_play_ns / 1e9, (unsigned long long)f_frames_play, f_play_ns ? f_frames_play / (f_play_ns / 1e9) : 0.0,
             f_keys, d_inputs);
    }
    return NULL;
}

/* The segment's fields for the p lines, after the stage fields. */
static void p_bench_fields(char *out, size_t size)
{
    static const char *const names[6] = { "guest_us", "copy_us", "keep_us", "flip_us", "frame_us", "present_us" };
    PwHistSummary frame, present, cpu;
    size_t n;
    char cpu_state[24], cpu_avg[24];

    pw_hist_summary(&p_snap[4], &frame);
    pw_hist_summary(&p_snap[5], &present);
    pw_hist_summary(&p_snap[6], &cpu);
    if (thread_cpu_errno == 0) snprintf(cpu_state, sizeof(cpu_state), "ok");
    else if (thread_cpu_errno == -1) snprintf(cpu_state, sizeof(cpu_state), "-");
    else snprintf(cpu_state, sizeof(cpu_state), "err:%d", thread_cpu_errno);
    n = (size_t)snprintf(out, size, " seg_s=%.0f warm_s=%d first_present_s=%.1f inputs=%d inputs_warm=%d pad_reads=%d "
                         "end=%s presents=%llu flush_hz=%.1f flushes=%llu noted=%llu full_flushes=%llu composed=%llu "
                         "dirty_px_avg=%llu dirty_px_max=%llu",
                         (double)(uint64_t)p_seg_s, P_WARM_S, p_first_s, p_inputs,
                         p_inputs_warm, p_pad_reads, p_end, (unsigned long long)p_snap_presents,
                         p_seg_s > 0 ? p_snap_presents / p_seg_s : 0.0, (unsigned long long)p_snap_flushes,
                         (unsigned long long)p_snap_noted, (unsigned long long)p_snap_full,
                         (unsigned long long)p_snap_composed,
                         (unsigned long long)(p_snap_flushes ? p_snap_dirty_sum / p_snap_flushes : 0),
                         (unsigned long long)p_snap_dirty_max);
    for (int i = 0; i < 4 && n < size; i++) {
        out[n++] = ' ';
        if (n < size) n += (size_t)pw_hist_fields(&p_snap[i], names[i], out + n, size - n);
    }
    if (cpu.n) snprintf(cpu_avg, sizeof(cpu_avg), "%llu", (unsigned long long)cpu.avg_us);
    else snprintf(cpu_avg, sizeof(cpu_avg), "-");
    if (n < size)
        n += (size_t)snprintf(out + n, size - n, " frame_us_avg=%llu present_us_avg=%llu guest_cpu_us_avg=%s "
                              "guest_cpu_n=%llu thread_cpu=%s thread_cpu_other=%llu",
                              (unsigned long long)frame.avg_us, (unsigned long long)present.avg_us, cpu_avg,
                              (unsigned long long)cpu.n, cpu_state, (unsigned long long)p_snap_cpu_other);
    if (n < size) {
        char shown[24], shown_hz[24];
        if (p_snap_shown >= 0) {
            snprintf(shown, sizeof(shown), "%lld", (long long)p_snap_shown);
            snprintf(shown_hz, sizeof(shown_hz), "%.1f", p_seg_s > 0 ? p_snap_shown / p_seg_s : 0.0);
        } else {
            snprintf(shown, sizeof(shown), "-");
            snprintf(shown_hz, sizeof(shown_hz), "-");
        }
        n += (size_t)snprintf(out + n, size - n, " present_path=%s present_mode=%s buffers=%u shown=%s shown_hz=%s "
                 "slot_waits=%llu flip_errors=%llu slot_timeouts=%llu status_errors=%llu", p_path_names[p_path],
                 p_async ? "async" : "sync", video.mode.buffers, shown, shown_hz,
                 (unsigned long long)p_snap_slot_waits, (unsigned long long)video.flip_errors,
                 (unsigned long long)video.slot_timeouts, (unsigned long long)video.status_errors);
    }
    if (n < size && p_audio >= 0) {
        /* The driver's counts (wine/wineps5 pw_wineps5_stats, src/pw_audio_mix.h): grains mixed, stream
         * grains that came up short, underruns (a stretch of them ended by more data while the stream
         * was started: silence inside a sound) and drains (ended by a stop: the sound's end). */
        uint64_t st[6] = { 0 };
        void *fn = NULL;
        const char *found = ntdll_symbol("pw_wineps5_stats", &fn);
        char v[6][24];
        for (int i = 0; i < 6; i++) snprintf(v[i], sizeof(v[i]), "-");
        if (!strcmp(found, "ok") && fn) {
            ((unsigned (*)(uint64_t *, unsigned))(uintptr_t)fn)(st, 6);
            for (int i = 0; i < 6; i++) snprintf(v[i], sizeof(v[i]), "%llu", (unsigned long long)st[i]);
        }
        snprintf(out + n, size - n, " audio_driver=ps5 grains=%llu grains_nonzero=%llu grains_tail=%llu underruns=%s "
                 "underrun_grains=%s drains=%s drain_grains=%s starved_grains=%s mixed_grains=%s audio_stats=%s "
                 "gaps=%llu gap_max_us=%llu audio_errors=%llu tail_keys=%d tail_s=%.1f midi=untested",
                 (unsigned long long)audio_grains, (unsigned long long)audio_nonzero,
                 (unsigned long long)(audio_nonzero - p_tail_nonzero0), v[2], v[3], v[4], v[5], v[1], v[0], found,
                 (unsigned long long)audio_gaps, (unsigned long long)(audio_gap_max_ns / 1000),
                 (unsigned long long)audio_errors, p_tail_keys, p_tail_s);
    }
}

/* ---- e1: RADV in libvulkan.prx, the display handed over ------------------------------ */

static void *(*ntdll_dlopen)(const char *, int);
static void *(*ntdll_dlsym)(void *, const char *);
static int32_t ntdll_module = -1;

/* An exit() inside a module during a title stage (RADV calls exit() on
 * fatal errors) ends the stage, not the title: the stage is marked, the
 * lines go out and the thread idles, as a hang does. The Wine stage
 * replaces this hook with the title's own. */
static void stage_exit_hook(void *context, int status, const char *kind)
{
    char detail[64];
    int stage = stage_active;

    (void)context;
    snprintf(detail, sizeof(detail), "exit=%d kind=%s", status, kind);
    if (stage >= 0) pw_stage_mark(&run, (unsigned)stage, 'F', detail);
    else cfg.emit(detail);
    pw_ms_emit_lines();
    for (;;) sleep(60);
}

/* What RADV reads from its environment (its own: libvulkan.prx is given
 * ntdll's when pw_wine_dl loads it), then <root>/env.txt, which may replace
 * any of them. */
static void vk_environment(int (*set)(const char *, const char *))
{
    char path[512], text[4096], *line, *next;
    FILE *file;
    size_t got;

    snprintf(path, sizeof(path), "%s/cache", cfg.root);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/cache/radv", cfg.root);
    mkdir(path, 0777);
    set("MESA_SHADER_CACHE_DIR", path);
    set("MESA_DISK_CACHE_DATABASE", "1");
    snprintf(path, sizeof(path), "%s/shot.raw", cfg.root);
    set("ORBIS_WSI_SHOT_PATH", path);
    set("ORBIS_WSI_SHOT", "300");
    snprintf(path, sizeof(path), "%s/env.txt", cfg.root);
    if (!(file = fopen(path, "rb"))) return;
    got = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[got] = 0;
    for (line = text; line && *line; line = next) {
        char *eq;
        if ((next = strchr(line, '\n'))) *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        if (*line == '#' || !(eq = strchr(line, '='))) continue;
        *eq = 0;
        set(line, eq + 1);
    }
}

/* ntdll as the Wine stage starts it: module_start, adopted by its loader
 * (a bare name is then found beside it), RADV's variables in its
 * environment and the stage exit hook. Once per process. */
static int ntdll_ready(char *why, size_t size)
{
    char path[512];
    const PwPrxDescriptor *descriptor = NULL;
    int (*start)(size_t, const void *);
    void *(*adopt)(const char *, int32_t);
    int (*set)(const char *, const char *);
    void (*set_hook)(PwWineExitHook, void *);
    Module m;
    int result;

    if (ntdll_dlopen) return 0;
    if ((ntdll_module = load("ntdll", &result)) <= 0) {
        snprintf(why, size, "ntdll:0x%x", (unsigned)ntdll_module);
        return -1;
    }
    if (module_info(ntdll_module, &m) || pw_prx_find_descriptor(m.segments, m.count, &descriptor) != PW_PRX_OK) {
        snprintf(why, size, "ntdll:nodescriptor");
        return -1;
    }
    start = (int (*)(size_t, const void *))(uintptr_t)pw_prx_lookup(descriptor, "module_start");
    adopt = (void *(*)(const char *, int32_t))(uintptr_t)pw_prx_lookup(descriptor, "pw_wine_dl_adopt");
    set = (int (*)(const char *, const char *))(uintptr_t)pw_prx_lookup(descriptor, "pw_wine_setenv");
    set_hook = (void (*)(PwWineExitHook, void *))(uintptr_t)pw_prx_lookup(descriptor, "pw_wine_set_exit_hook");
    if (!start || !adopt || !set || !set_hook) { snprintf(why, size, "ntdll:noexports"); return -1; }
    start(0, NULL);
    snprintf(path, sizeof(path), "%s/ntdll.prx", cfg.module_dir);
    if (!adopt(path, ntdll_module)) { snprintf(why, size, "ntdll:adopt"); return -1; }
    vk_environment(set);
    set_hook(stage_exit_hook, NULL);
    ntdll_dlsym = (void *(*)(void *, const char *))(uintptr_t)pw_prx_lookup(descriptor, "dlsym");
    ntdll_dlopen = (void *(*)(const char *, int))(uintptr_t)pw_prx_lookup(descriptor, "dlopen");
    if (!ntdll_dlopen || !ntdll_dlsym) { ntdll_dlopen = NULL; snprintf(why, size, "ntdll:nodlfcn"); return -1; }
    return 0;
}

static void vk_step(const char *line) { step("%s", line); }

/* libvulkan.prx through ntdll's dlopen, as win32u opens it. */
static int vk_ready(char *why, size_t size)
{
    PwVkHost host = { NULL, NULL, vk_step, NULL };
    if (ntdll_ready(why, size)) return -1;
    host.dlopen = ntdll_dlopen;
    host.dlsym = ntdll_dlsym;
    host.now_ns = cfg.now_ns;
    return pw_vk_open(&host, why, size);
}

static void stage_vk_load(char *fields, size_t size)
{
    unsigned long long dmem_before, flex_before;
    uint64_t t0 = cfg.now_ns();
    int32_t handle;
    char why[96];
    Module m;
    Slots s;

    pw_vk_memory(&dmem_before, &flex_before);
    if (vk_ready(why, sizeof(why))) {
        snprintf(fields, size, "vk_load=fail:%s vk_bound=-", why);
        return;
    }
    {
        unsigned long long dmem_after, flex_after, load_ms = (cfg.now_ns() - t0) / 1000000ull;
        pw_vk_memory(&dmem_after, &flex_after);
        /* pw_wine_dl loaded it; its handle by name (loading the same path again returns it too). */
        if ((handle = module_handle("libvulkan")) < 0) {
            int result;
            handle = load("libvulkan", &result);
        }
        if (handle <= 0 || module_info(handle, &m)) {
            snprintf(fields, size, "vk_load=ok vk_bound=nomodule load_ms=%llu", load_ms);
            return;
        }
        check_slots("libvulkan", &m, ntdll_module, &s, NULL);
        slot_step("vk_load", &s);
        snprintf(fields, size, "vk_load=ok vk_bound=%u/%u vk_first=%s load_ms=%llu vk_kib=%llu flex_before_mib=%llu "
                 "flex_after_mib=%llu dmem_load_before_mib=%llu dmem_load_after_mib=%llu", s.bound, s.total,
                 s.first[0] ? s.first : "-", load_ms, (unsigned long long)((m.high - m.low) >> 10), flex_before,
                 flex_after, dmem_before, dmem_after);
    }
}

static void stage_vk_caps(char *fields, size_t size)
{
    char why[96];
    if (vk_ready(why, sizeof(why))) { snprintf(fields, size, "inst=fail:%s", why); return; }
    pw_vk_caps(fields, size);
}

/* The title's VideoOut closed and the GDI present sink stopped: from here
 * the display is RADV's (wsi_orbis.c opens port 255 at swapchain creation). */
static pthread_t main_thread;
static volatile int release_request, release_result;
static char release_state[24] = "-";

static int release_display(void)
{
    int rc;
    pthread_mutex_lock(&present_lock);
    rc = video_open ? pw_videoout_orbis_close(&video) : 0;
    if (!rc) video_open = 0;
    pthread_mutex_unlock(&present_lock);
    snprintf(release_state, sizeof(release_state), rc ? "fail:0x%x" : "ok", (unsigned)rc);
    step("display released rc=0x%x", (unsigned)rc);
    return rc;
}

/* ntdll's display release (pw_wine_set_display_release), called by win32u
 * before its first surface (patch 0461). The port is closed on the thread
 * that opened it, the title's main thread (pw_ms_service), as the PS5 title
 * does: a close from another thread is not measured. 0 once closed. */
int pw_ms_release_display(void *context)
{
    (void)context;
    if (pthread_equal(pthread_self(), main_thread)) return release_display();
    __atomic_store_n(&release_request, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 500 && __atomic_load_n(&release_request, __ATOMIC_ACQUIRE); i++) pause_ms(10);
    if (__atomic_load_n(&release_request, __ATOMIC_ACQUIRE)) {
        snprintf(release_state, sizeof(release_state), "timeout");
        return -1;
    }
    return release_result;
}

void pw_ms_service(void)
{
    if (!__atomic_load_n(&release_request, __ATOMIC_ACQUIRE)) return;
    release_result = release_display();
    __atomic_store_n(&release_request, 0, __ATOMIC_RELEASE);
}

static void stage_release(char *fields, size_t size)
{
    int rc;
    /* A resumed launch is a new process that has not opened the port. */
    if (!video_open) { snprintf(fields, size, "release=notopen"); return; }
    rc = release_display();
    snprintf(fields, size, "release=%s release_rc=0x%x", rc ? "fail" : "ok", (unsigned)rc);
}

static void stage_vk_present(char *fields, size_t size)
{
    char why[96], shot[512];
    if (vk_ready(why, sizeof(why))) { snprintf(fields, size, "present=fail:%s gpu_hang=0", why); return; }
    snprintf(shot, sizeof(shot), "%s/shot.raw", cfg.root);
    pw_vk_present(shot, fields, size);
}

/* gc1: VideoOut's hardware cursor on the title's own display (pw_cursor_probe_orbis.h). */
static void stage_cursor(char *fields, size_t size)
{
    PwCursorProbeHost host = { vk_step, cfg.now_ns };
    pw_cursor_probe_videoout(&host, fields, size);
}

/* gc2: libvulkan.prx's pw_videoout_cursor over e1's frames. */
static void stage_vk_cursor(char *fields, size_t size)
{
    PwCursorProbeHost host = { vk_step, cfg.now_ns };
    char why[96], shot[512];
    if (vk_ready(why, sizeof(why))) {
        snprintf(fields, size, "present=fail:%s gpu_hang=0 cursor_export=-", why);
        return;
    }
    snprintf(shot, sizeof(shot), "%s/shot.raw", cfg.root);
    pw_cursor_probe_vulkan(&host, shot, fields, size);
}

/* ---- e2 + e3: winevulkan, win32u's surface, the hand-over through Wine ------------------- */

static int vulkan_plan(void);
static int dxvk_plan(void);
static int pinball_plan(void);

/* winevulkan.prx's slots against both its exporters (ntdll, then win32u for
 * __wine_get_vulkan_driver). */
static void stage_wv_bind(char *fields, size_t size)
{
    int result;
    int32_t ntdll = load("ntdll", &result), win32u, wv;
    Exporter exporters[2];
    Module m;
    Slots s;

    if (ntdll <= 0) { snprintf(fields, size, "wv_load=nontdll wv_bound=-"); return; }
    if ((win32u = load("win32u", &result)) <= 0) { snprintf(fields, size, "wv_load=nowin32u wv_bound=-"); return; }
    wv = load("winevulkan", &result);
    step("wv_bind winevulkan load=0x%x result=%d", (unsigned)wv, result);
    if (wv <= 0) { snprintf(fields, size, "wv_load=0x%x wv_bound=-", (unsigned)wv); return; }
    if (module_info(wv, &m)) { snprintf(fields, size, "wv_load=ok wv_bound=nomoduleinfo"); return; }
    exporters[0] = (Exporter){ "ntdll", ntdll };
    exporters[1] = (Exporter){ "win32u", win32u };
    check_slots_from("winevulkan", &m, exporters, 2, &s, "__wine_get_vulkan_driver");
    slot_step("wv_bind", &s);
    snprintf(fields, size, "wv_load=ok wv_bound=%u/%u wv_first=%s wv_driver=%s", s.bound, s.total,
             s.first[0] ? s.first : "-", s.watch_state);
}

/* ---- f1: wowprospero.prx, the address space below 4 GiB ------------------------------------ */

/* wowprospero.prx's slots, all from ntdll (the hooks of patches 0610/0710,
 * __wine_ps5_mprotect, the Nt* calls); the unresolved-fault hook's own state. */
static void stage_wow_bind(char *fields, size_t size)
{
    int result;
    int32_t ntdll = load("ntdll", &result), wow;
    void *stats = NULL;
    Module m;
    Slots s;

    if (ntdll <= 0) { snprintf(fields, size, "wow_load=nontdll wow_bound=-"); return; }
    wow = load("wowprospero", &result);
    step("wow_bind wowprospero load=0x%x result=%d", (unsigned)wow, result);
    if (wow <= 0) { snprintf(fields, size, "wow_load=0x%x wow_bound=-", (unsigned)wow); return; }
    if (module_info(wow, &m)) { snprintf(fields, size, "wow_load=ok wow_bound=nomoduleinfo"); return; }
    check_slots("wowprospero", &m, ntdll, &s, "__wine_ps5_set_segv_unresolved_hook");
    slot_step("wow_bind", &s);
    sceKernelDlsym(wow, "pw_wow_stats", &stats);
    snprintf(fields, size, "wow_load=ok wow_bound=%u/%u wow_first=%s wow_hook=%s wow_sym=%d", s.bound, s.total,
             s.first[0] ? s.first : "-", s.watch_state, stats != NULL);
}

/* What is mapped between 64 KiB and 4 GiB before Wine reserves anything:
 * Wine's i386 side needs its images and heaps there (patches 0601, 0895).
 * The free total, the largest hole, the number of regions, and the first
 * few regions in the log. */
static void stage_low_map(char *fields, size_t size)
{
    const uint64_t top = 1ull << 32;
    uint64_t at = 0x10000, free_bytes = 0, hole_base = 0, hole_size = 0, used = 0;
    unsigned maps = 0;

    while (at < top) {
        VqInfo info;
        uint64_t start, end;

        memset(&info, 0, sizeof(info));
        if (sceKernelVirtualQuery((const void *)(uintptr_t)at, 1, &info, sizeof(info)) != 0) break;
        start = (uint64_t)(uintptr_t)info.start;
        end = (uint64_t)(uintptr_t)info.end;
        if (end <= start || end <= at) break;
        if (start >= top) break;
        if (start > at) {
            free_bytes += start - at;
            if (start - at > hole_size) { hole_size = start - at; hole_base = at; }
        }
        if (maps < 8)
            step("low_map region 0x%llx-0x%llx prot=0x%x type=%d kind=0x%x name=%.32s", (unsigned long long)start,
                 (unsigned long long)end, (unsigned)info.protection, info.type, info.kind & 0x1f, info.name);
        maps++;
        used += (end < top ? end : top) - (start > at ? start : at);
        at = end;
    }
    if (at < top) {
        free_bytes += top - at;
        if (top - at > hole_size) { hole_size = top - at; hole_base = at; }
    }
    snprintf(fields, size, "low_map=ok low_free_mib=%llu low_used_mib=%llu low_maps=%u low_hole=0x%llx+%llu",
             (unsigned long long)(free_bytes >> 20), (unsigned long long)(used >> 20), maps,
             (unsigned long long)hole_base, (unsigned long long)(hole_size >> 20));
}

/* The Wine program's own lines (vk64.exe: VK64, VK64S; d3d11tri64.exe:
 * D3D11; wow32.exe: HELLO32, HELLO32S, GDI32), kept whole: the title
 * remembers only the last stdout line. */
static char vk64_line[256], vk64s_line[256], d3d11_line[256];
static char hello32_line[160], hello32s_line[160], gdi32_line[320];
static int hello32_done;
/* p5w, p6: wincb.exe's summary (WINCB done); its hold markers are sampled as they come. */
static char wincb_done[400];

void pw_ms_stdout(const char *line)
{
    if (!strncmp(line, "VK64 ", 5)) snprintf(vk64_line, sizeof(vk64_line), "%s", line + 5);
    else if (!strncmp(line, "VK64S ", 6)) snprintf(vk64s_line, sizeof(vk64s_line), "%s", line + 6);
    else if (!strncmp(line, "D3D11 ", 6)) snprintf(d3d11_line, sizeof(d3d11_line), "%s", line + 6);
    else if (!strncmp(line, "D3D9 ", 5)) snprintf(d3d11_line, sizeof(d3d11_line), "%s", line + 5);
    else if (!strncmp(line, "HELLO32 ", 8)) snprintf(hello32_line, sizeof(hello32_line), "%s", line + 8);
    else if (!strncmp(line, "HELLO32S ", 9)) snprintf(hello32s_line, sizeof(hello32s_line), "%s", line + 9);
    else if (!strncmp(line, "GDI32 ", 6)) snprintf(gdi32_line, sizeof(gdi32_line), "%s", line + 6);
    else if (!strcmp(line, "Hello from PE32")) hello32_done = 1;
    else if (!strncmp(line, "WINCB done ", 11)) snprintf(wincb_done, sizeof(wincb_done), "%s", line + 11);
    else if (!strcmp(line, "WINCB hold_begin")) hold_sample(0);
    else if (!strncmp(line, "WINCB hold n=", 13)) hold_sample(1);
    else if (!strncmp(line, "WINCB hold_end", 14)) hold_sample(2);
    else if (!strncmp(line, "WINCB hold2 n=", 14)) hold_sample(3);
    else if (!strncmp(line, "WINCB hold2_end", 15)) hold_sample(4);
}

/* What RADV reads (libvulkan.prx is given ntdll's environment when win32u
 * loads it) and a desktop the size of the swapchain, for the Wine stage of
 * the plans that run Vulkan; <root>/env.txt is read after, and wins. */
void pw_ms_environment(void (*add)(const char *name, const char *value))
{
    static char cache[512], shot[512];
    /* Milestone f: Pinball's window (about 600x470) fits 960x540, which fills
     * 1920x1080 at twice its size. */
    if (pinball_plan()) add("WINE_PS5_DESKTOP", bench_plan && p_path == P_PATH_VO720 ? "1280x720" : "960x540");
    /* p2: 32-bit code on the hardware backend (patch 0611); wow64 falls back to
     * the prefix's wowprospero.dll when it does not start (0614). */
    if (native_plan()) add("WINE_PS5_WOW64_CPU", "wow64native.dll");
    if (g1_plans() || g_plan()) {
        /* Milestone g: the native CPU (g's comes from its profile; env.txt's
         * WINE_PS5_WOW64_CPU=wowprospero.dll chooses the translator), Vulkan
         * batching, and its counters on every 60th present (the title reads
         * them in pw_ms_wine_line). */
        if (g1_plans()) add("WINE_PS5_WOW64_CPU", "wow64native.dll");
        add("PW_VK_BATCH", "1");
        add("PW_VK_BATCH_STATS", "1");
        add("PW_VK_BATCH_STATS_EVERY", "60");
    }
    if (!vulkan_plan()) return;
    snprintf(cache, sizeof(cache), "%s/cache", cfg.root);
    mkdir(cache, 0777);
    snprintf(cache, sizeof(cache), "%s/cache/radv", cfg.root);
    mkdir(cache, 0777);
    snprintf(shot, sizeof(shot), "%s/shot.raw", cfg.root);
    add("MESA_SHADER_CACHE_DIR", cache);
    add("MESA_DISK_CACHE_DATABASE", "1");
    /* Not in g: writing the shot costs the present a few hundred ms (e's "long scan-out copy"). */
    if (!g_plan()) {
        add("ORBIS_WSI_SHOT_PATH", shot);
        add("ORBIS_WSI_SHOT", "300");
    }
    add("WINE_PS5_DESKTOP", "1920x1080");
    /* Milestone e: DXVK 2.6.2's d3d11.dll and dxgi.dll in the prefix's system32 (prefix.sh --extras). */
    if (dxvk_plan()) add("WINEDLLOVERRIDES", "d3d11,dxgi=n");
    /* g1: DXVK 2.6.2's 32-bit d3d9.dll in syswow64 (prefix.sh), with the three settings of its
     * built-in vampire.exe profile, which d3d9tri32.exe's name does not match. */
    if (plan->lines == g1_lines) {
        add("WINEDLLOVERRIDES", "d3d9=n");
        add("DXVK_CONFIG_FILE", "C:\\g1\\dxvk.conf");
    }
}

/* libvulkan.prx's present counters, through ntdll's dlsym of a name any
 * loaded module's descriptor exports; "ok", or why there are none (the
 * counters are then zeros). ntdll's handle by name, else by loading its
 * path again (the first e2 launch read zeros: the name look-up is
 * unmeasured, the path load is what e1's stages use). */
static const char *ntdll_symbol(const char *name, void **address)
{
    const PwPrxDescriptor *descriptor = NULL;
    void *(*sym)(void *, const char *);
    int32_t ntdll = module_handle("ntdll");
    Module m;
    int result;

    *address = NULL;
    if (ntdll < 0) ntdll = load("ntdll", &result);
    if (ntdll <= 0 || module_info(ntdll, &m)) return "nontdll";
    step("%s: ntdll handle=0x%x name=%s", name, (unsigned)ntdll, m.name);
    if (pw_prx_find_descriptor(m.segments, m.count, &descriptor) != PW_PRX_OK) return "nodescriptor";
    if (!(sym = (void *(*)(void *, const char *))(uintptr_t)pw_prx_lookup(descriptor, "dlsym"))) return "nodlsym";
    if (!(*address = sym(NULL, name))) return "nosym";
    return "ok";
}

static const char *vulkan_stats(struct pw_vulkan_stats *stats)
{
    void *address;
    const char *found = ntdll_symbol("pw_vulkan_stats", &address);

    memset(stats, 0, sizeof(*stats));
    stats->size = sizeof(*stats);
    if (strcmp(found, "ok")) return found;
    return ((int (*)(struct pw_vulkan_stats *))(uintptr_t)address)(stats) ? "readfailed" : "ok";
}

/* wowprospero's counters (wow_stats.h) from the instance Wine loaded, found
 * through ntdll's dlsym as libvulkan's are; zeros and the reason otherwise. */
static const char *wow_stats(uint64_t out[PW_WOW_STAT_COUNT])
{
    void *address;
    const char *found = ntdll_symbol("pw_wow_stats", &address);

    memset(out, 0, sizeof(uint64_t) * PW_WOW_STAT_COUNT);
    if (strcmp(found, "ok")) return found;
    ((pw_wow_stats_fn)(uintptr_t)address)(out, PW_WOW_STAT_COUNT);
    return "ok";
}

/* ---- thread stacks: ntdll.prx's pthread layer and the free flexible memory ---- */

/* Free flexible memory in bytes, -1 when the kernel does not say. */
static long long flex_free(void)
{
    size_t size = 0;
    return sceKernelAvailableFlexibleMemorySize(&size) ? -1 : (long long)size;
}

typedef struct ThreadStats {
    const char *found;   /* "ok", or why there are no counts */
    unsigned long created, raised, create_failed;
    unsigned long long floor;
    const char *policy;  /* the caller-stack policy, NULL when ntdll has no such export */
    unsigned long kept, small, keep_refused;
} ThreadStats;

/* The pthread layer linked into ntdll.prx (orbis-compat's, which gives a
 * thread asking for less at least the main thread's stack): its counts,
 * through ntdll's export (wine/orbis/pw_wine_threads_orbis.c). The title's
 * own copy of the layer counts only the title's threads. */
static void thread_stats(ThreadStats *t)
{
    void *address;

    memset(t, 0, sizeof(*t));
    t->found = ntdll_symbol("pw_wine_thread_stats", &address);
    if (!strcmp(t->found, "ok"))
        ((void (*)(unsigned long *, unsigned long *, unsigned long *, unsigned long long *))(uintptr_t)address)(
            &t->created, &t->raised, &t->create_failed, &t->floor);
    /* Threads whose caller set the stack (spawn_thread's kernel stack): kept,
     * given the layer's small stack, refused (patch 0001 to the layer). */
    if (!strcmp(ntdll_symbol("pw_wine_thread_caller_stacks", &address), "ok"))
        ((void (*)(unsigned long *, unsigned long *, unsigned long *, const char **))(uintptr_t)address)(
            &t->kept, &t->small, &t->keep_refused, &t->policy);
}

static long long flex_start = -1;   /* before Wine (pw_ms_wine_begin) */

/* flex_mib_start flex_mib threads_created threads_raised threads_create_failed stack_floor_mib thread_stats
 * stack_policy threads_kept threads_small threads_keep_refused */
static void thread_fields(char *out, size_t size)
{
    ThreadStats t;
    long long flex = flex_free();
    char start[24] = "-", now[24] = "-", callers[160];
    size_t n;

    thread_stats(&t);
    if (flex_start >= 0) snprintf(start, sizeof(start), "%lld", flex_start >> 20);
    if (flex >= 0) snprintf(now, sizeof(now), "%lld", flex >> 20);
    if (strcmp(t.found, "ok"))
        n = snprintf(out, size, "flex_mib_start=%s flex_mib=%s threads_created=- threads_raised=- "
                     "threads_create_failed=- stack_floor_mib=- thread_stats=%s", start, now, t.found);
    else
        n = snprintf(out, size, "flex_mib_start=%s flex_mib=%s threads_created=%lu threads_raised=%lu "
                     "threads_create_failed=%lu stack_floor_mib=%llu thread_stats=ok", start, now, t.created,
                     t.raised, t.create_failed, t.floor >> 20);
    if (t.policy)
        snprintf(callers, sizeof(callers), "stack_policy=%s threads_kept=%lu threads_small=%lu threads_keep_refused=%lu",
                 t.policy, t.kept, t.small, t.keep_refused);
    else
        snprintf(callers, sizeof(callers), "stack_policy=- threads_kept=- threads_small=- threads_keep_refused=-");
    if (n < size) snprintf(out + n, size - n, " %s", callers);
}

/* p5w's and p6's hold probe: the free flexible memory when wincb.exe
 * announces its 8 threads (0 hold_begin), when all 8 are alive (1 hold n=8),
 * when they have ended (2 hold_end), when 8 more are alive (3 hold2 n=8) and
 * when those have ended (4 hold2_end), and the layer's thread count at 1.
 * Called on the stdout forwarder's thread. */
static long long hold_flex[5] = { -1, -1, -1, -1, -1 };
static ThreadStats hold_threads;

static void hold_sample(int at)
{
    if (at < 0 || at > 4) return;
    hold_flex[at] = flex_free();
    if (at == 1) thread_stats(&hold_threads);
}

/* KiB from sample A down to sample B ("-" if either is missing). */
static void hold_kib(char *out, size_t size, int a, int b)
{
    if (hold_flex[a] >= 0 && hold_flex[b] >= 0) snprintf(out, size, "%lld", (hold_flex[a] - hold_flex[b]) / 1024);
    else snprintf(out, size, "-");
}

/* hold_flex_kib (begin minus alive: what the first 8 threads took),
 * hold_back_kib (what came back when they ended: end minus alive),
 * hold2_flex_kib (what the second 8 took after the first had ended: about 0
 * when stacks are reused), flex_mib_hold, threads_created_hold */
static void hold_fields(char *out, size_t size)
{
    char kib[24], back[24], kib2[24], mib[24] = "-", created[24] = "-";

    hold_kib(kib, sizeof(kib), 0, 1);
    hold_kib(back, sizeof(back), 2, 1);
    hold_kib(kib2, sizeof(kib2), 2, 3);
    if (hold_flex[1] >= 0) snprintf(mib, sizeof(mib), "%lld", hold_flex[1] >> 20);
    if (hold_threads.found && !strcmp(hold_threads.found, "ok")) snprintf(created, sizeof(created), "%lu", hold_threads.created);
    snprintf(out, size, "hold_flex_kib=%s hold_back_kib=%s hold2_flex_kib=%s flex_mib_hold=%s threads_created_hold=%s",
             kib, back, kib2, mib, created);
}

/* libvulkan.prx's own copy of the pthread layer (RADV's threads): created,
 * raised and the floor it resolved, read through libvulkan's descriptor
 * (pw_vulkan_thread_stats) so ntdll's copy is not the one found. The floor
 * is "-" when libvulkan created no thread (it is then not resolved). */
static void vk_thread_fields(char *out, size_t size)
{
    const PwPrxDescriptor *descriptor = NULL;
    void (*get)(unsigned long *, unsigned long *, size_t *);
    unsigned long created = 0, raised = 0;
    size_t floor = 0;
    int32_t handle = module_handle("libvulkan");
    Module m;
    const char *why = NULL;

    if (handle <= 0 || module_info(handle, &m)) why = "nomodule";
    else if (pw_prx_find_descriptor(m.segments, m.count, &descriptor) != PW_PRX_OK) why = "nodescriptor";
    else if (!(get = (void (*)(unsigned long *, unsigned long *, size_t *))(uintptr_t)pw_prx_lookup(
                   descriptor, "pw_vulkan_thread_stats"))) why = "nosym";
    else get(&created, &raised, &floor);
    if (why)
        snprintf(out, size, "vk_threads_created=- vk_threads_raised=- vk_stack_floor_kib=- vk_thread_stats=%s", why);
    else if (!floor)
        snprintf(out, size, "vk_threads_created=%lu vk_threads_raised=%lu vk_stack_floor_kib=- vk_thread_stats=ok",
                 created, raised);
    else
        snprintf(out, size, "vk_threads_created=%lu vk_threads_raised=%lu vk_stack_floor_kib=%zu vk_thread_stats=ok",
                 created, raised, floor >> 10);
}

/* The value of KEY in a "k=v k=v" line, "-" when absent. */
static const char *pick(const char *line, const char *key, char *out, size_t size)
{
    size_t n = strlen(key);
    for (const char *p = line; p && *p; p = strchr(p, ' ') ? strchr(p, ' ') + 1 : NULL) {
        if (!strncmp(p, key, n) && p[n] == '=') {
            const char *v = p + n + 1, *end = strchr(v, ' ');
            snprintf(out, size, "%.*s", (int)(end ? (size_t)(end - v) : strlen(v)), v);
            return out;
        }
    }
    snprintf(out, size, "-");
    return out;
}

/* "key=value ..." with each KEY of RENAME (pairs: from, to) renamed. */
static void renamed(const char *in, const char *const *rename, size_t pairs, char *out, size_t size)
{
    size_t n = 0;
    out[0] = 0;
    for (const char *p = in; *p && n + 1 < size;) {
        const char *end = strchr(p, ' '), *eq = strchr(p, '=');
        size_t word = end ? (size_t)(end - p) : strlen(p), key = eq && eq < p + word ? (size_t)(eq - p) : word;
        const char *to = NULL;
        for (size_t i = 0; i < pairs && !to; i++)
            if (strlen(rename[2 * i]) == key && !strncmp(p, rename[2 * i], key)) to = rename[2 * i + 1];
        n += (size_t)snprintf(out + n, size - n, "%s%s%.*s", n ? " " : "", to ? to : "", (int)(to ? word - key : word),
                              to ? p + key : p);
        p += word;
        while (*p == ' ') p++;
    }
}

/* ---- milestone g: CPU time per thread (recorded) ------------------------------------ */

/* The kernel's per-thread records: sysctl(CTL_KERN, KERN_PROC, KERN_PROC_PID |
 * KERN_PROC_INC_THREAD, pid) gives one struct kinfo_proc per thread, with its id
 * (ki_tid), name (ki_tdname: what libthr's rename set) and run time (ki_runtime,
 * microseconds). The SDK has no kinfo_proc and FreeBSD 9's offsets are not
 * trusted here: cpu_probe measures them on a thread it names and keeps busy for
 * CPU_PROBE_US, and the census uses what it found (or stays off: "-"). */
extern int sysctl(const int *name, unsigned namelen, void *old, size_t *oldlen, const void *new_value,
                  size_t newlen);
extern void *scePthreadSelf(void);
extern int scePthreadRename(void *thread, const char *name);
extern int pthread_getthreadid_np(void);
enum { K_CTL_KERN = 1, K_KERN_PROC = 14, K_KERN_PROC_PID = 1, K_KERN_PROC_INC_THREAD = 0x10,
       CPU_PROBE_US = 200000, CENSUS_BYTES = 256 * 1024, CENSUS_NAME = 16 };
static uint8_t census_buf[CENSUS_BYTES];
static pthread_mutex_t census_lock = PTHREAD_MUTEX_INITIALIZER;
static int census_size, census_name_off = -1, census_runtime_off = -1, census_tid_off = -1;

/* The records into census_buf; how many, or -1 (errno kept). */
static int census_read(size_t *record)
{
    int mib[4] = { K_CTL_KERN, K_KERN_PROC, K_KERN_PROC_PID | K_KERN_PROC_INC_THREAD, (int)getpid() };
    size_t length = sizeof(census_buf);
    int size;

    if (sysctl(mib, 4, census_buf, &length, NULL, 0) != 0 || length < sizeof(int)) return -1;
    memcpy(&size, census_buf, sizeof(size));   /* ki_structsize */
    if (size < 512 || size > 4096) { errno = EPROTO; return -1; }
    *record = (size_t)size;
    return (int)(length / (size_t)size);
}

/* Thread CPU through getrusage(RUSAGE_THREAD), in microseconds (p1's measure). */
static uint64_t self_cpu_us(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_THREAD, &ru)) return 0;
    return (uint64_t)ru.ru_utime.tv_sec * 1000000u + (uint64_t)ru.ru_utime.tv_usec +
           (uint64_t)ru.ru_stime.tv_sec * 1000000u + (uint64_t)ru.ru_stime.tv_usec;
}

static volatile int probe_tid, probe_state;   /* 1 spun, 2 may end */
static volatile uint64_t probe_self_us;

static void *probe_spin(void *arg)
{
    volatile uint64_t sink = 0;
    uint64_t start;
    (void)arg;
    scePthreadRename(scePthreadSelf(), "pwcpuprobe");
    probe_tid = pthread_getthreadid_np();
    start = self_cpu_us();
    while (self_cpu_us() - start < CPU_PROBE_US)
        for (int i = 0; i < 10000; i++) sink += (uint64_t)i;
    probe_self_us = self_cpu_us();
    __atomic_store_n(&probe_state, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&probe_state, __ATOMIC_ACQUIRE) != 2) pause_ms(5);
    return NULL;
}

/* A title stage: does the kernel report another thread's run time, and where
 * in its record are the name, the id and the run time? */
static void stage_cpu_probe(char *fields, size_t size)
{
    pthread_t thread;
    size_t record = 0;
    int count, found = -1, waited = 0;

    if (pthread_create(&thread, NULL, probe_spin, NULL)) { snprintf(fields, size, "cpu_probe=nothread"); return; }
    while (__atomic_load_n(&probe_state, __ATOMIC_ACQUIRE) != 1 && waited++ < 600) pause_ms(5);
    count = census_read(&record);
    if (count > 0) {
        for (int i = 0; i < count && found < 0; i++) {
            const uint8_t *r = census_buf + (size_t)i * record;
            for (size_t off = 0; off + 11 <= record; off++)
                if (!memcmp(r + off, "pwcpuprobe", 11)) { census_name_off = (int)off; found = i; break; }
        }
    }
    if (found >= 0) {
        const uint8_t *r = census_buf + (size_t)found * record;
        uint64_t self = probe_self_us, v;
        int32_t t;
        /* ki_runtime: FreeBSD 9's place first, else any u64 that agrees with the thread's own clock. */
        memcpy(&v, r + 328, 8);
        if (record > 336 && v >= self * 8 / 10 && v <= self * 13 / 10) census_runtime_off = 328;
        for (size_t off = 0; census_runtime_off < 0 && off + 8 <= record; off += 8) {
            memcpy(&v, r + off, 8);
            if (v >= self * 8 / 10 && v <= self * 13 / 10) census_runtime_off = (int)off;
        }
        for (size_t off = 96; off + 4 <= record; off += 4) {
            memcpy(&t, r + off, 4);
            if (t == probe_tid) { census_tid_off = (int)off; break; }
        }
        census_size = (int)record;
    }
    __atomic_store_n(&probe_state, 2, __ATOMIC_RELEASE);
    pthread_join(thread, NULL);
    {
        uint64_t runtime = 0;
        if (found >= 0 && census_runtime_off >= 0)
            memcpy(&runtime, census_buf + (size_t)found * record + census_runtime_off, 8);
        snprintf(fields, size, "cpu_probe=%s census_threads=%d census_size=%zu name_off=%d runtime_off=%d tid_off=%d "
                 "probe_runtime_us=%llu probe_self_us=%llu",
                 count < 0 ? "nosysctl" : found < 0 ? "noname" : census_runtime_off < 0 ? "noruntime" : "ok",
                 count, record, census_name_off, census_runtime_off, census_tid_off, (unsigned long long)runtime,
                 (unsigned long long)probe_self_us);
        if (count < 0) { size_t n = strlen(fields); snprintf(fields + n, size - n, " census_errno=%d", errno); }
    }
    if (found < 0 || census_runtime_off < 0) census_size = 0;   /* the census stays off */
}

/* The run time of the threads by role: Wine's main thread (wine-main: the
 * game's main thread), DXVK's CS thread (dxvk-cs), its submission thread
 * (dxvk-submit, which presents), and all others. 0, or -1 without a census. */
enum { ROLE_MAIN, ROLE_CS, ROLE_SUBMIT, ROLE_OTHER, ROLES };
static int census_roles(uint64_t out[ROLES])
{
    size_t record;
    int count;

    memset(out, 0, sizeof(uint64_t) * ROLES);
    if (!census_size) return -1;
    pthread_mutex_lock(&census_lock);
    count = census_read(&record);
    for (int i = 0; count > 0 && (size_t)census_size == record && i < count; i++) {
        const uint8_t *r = census_buf + (size_t)i * record;
        char name[CENSUS_NAME + 1];
        uint64_t runtime;
        memcpy(name, r + census_name_off, CENSUS_NAME);
        name[CENSUS_NAME] = 0;
        memcpy(&runtime, r + census_runtime_off, 8);
        if (!strcmp(name, "wine-main")) out[ROLE_MAIN] += runtime;
        else if (!strncmp(name, "dxvk-cs", 7)) out[ROLE_CS] += runtime;
        else if (!strncmp(name, "dxvk-submit", 11)) out[ROLE_SUBMIT] += runtime;
        else out[ROLE_OTHER] += runtime;
    }
    pthread_mutex_unlock(&census_lock);
    return count > 0 && (size_t)census_size == record ? 0 : -1;
}

/* ---- milestone g: the profile, Wine's lines, the window --------------------------- */

/* The game's profile the title loaded (wine64_main_orbis.c), for the input preset and the CPU. */
static const PwGameProfile *g_profile;
static char g_profile_id[PW_APP_ID_CAPACITY] = "-", g_profile_status[48] = "-";

void pw_ms_set_profile(const PwGameProfile *profile, const char *id, const char *status)
{
    g_profile = profile;
    snprintf(g_profile_id, sizeof(g_profile_id), "%s", id && id[0] ? id : "-");
    snprintf(g_profile_status, sizeof(g_profile_status), "%s", status && status[0] ? status : "-");
}

/* 32-bit winevulkan's counters (wine/ps5/vulkan/pw_vk_batch_pe.c, PW_VK_BATCH_STATS=1):
 * cumulative, so the difference between any two records is exact whatever
 * PW_VK_BATCH_STATS_EVERY drops between them. */
typedef struct BatchRec {
    uint64_t present, records, crossings, ns;
    int enabled, negotiated, seen;
} BatchRec;
static pthread_mutex_t batch_lock = PTHREAD_MUTEX_INITIALIZER;
static BatchRec batch_first, batch_last;
static uint64_t batch_lines;
/* Wine's reports of an exception nobody handled: on the native CPU a guest
 * fault that the guest's SEH did not catch ends there, so this is g's
 * unresolved-fault count beside the translator's own. */
static uint32_t wine_unhandled;
/* DXVK's out-of-memory reports (memoryTrackTest makes D3D9 refuse images). */
static uint32_t dxvk_oom;
/* +loaddll: where the game's program landed (0x400000 is the eboot's), its
 * UP loader plugins (.vtm), Bink and Miles. */
static char exe_base[24] = "-";
static uint32_t g_vtm, g_bink, g_miles;

static uint64_t field_u64(const char *line, const char *key, int *found)
{
    const char *p = strstr(line, key);
    *found = p != NULL;
    return p ? strtoull(p + strlen(key), NULL, 10) : 0;
}

void pw_ms_wine_line(const char *line)
{
    const char *p;

    if ((p = strstr(line, "PW_VK_BATCH version=1 "))) {
        BatchRec r = { 0 };
        int found, all = 1;
        r.present = field_u64(p, " present=", &found); all &= found;
        r.records = field_u64(p, " records=", &found); all &= found;
        r.crossings = field_u64(p, " wine_unix_crossings=", &found); all &= found;
        r.enabled = (int)field_u64(p, " enabled=", &found); all &= found;
        r.negotiated = (int)field_u64(p, " negotiated=", &found); all &= found;
        if (!all) return;
        r.ns = cfg.now_ns ? cfg.now_ns() : 0;
        r.seen = 1;
        pthread_mutex_lock(&batch_lock);
        if (!batch_first.seen) batch_first = r;
        batch_last = r;
        batch_lines++;
        pthread_mutex_unlock(&batch_lock);
        return;
    }
    if (strstr(line, "Unhandled exception") || strstr(line, "wine: Unhandled") || strstr(line, "Unhandled page fault"))
        __atomic_add_fetch(&wine_unhandled, 1, __ATOMIC_RELAXED);
    if (strstr(line, "OUTOFVIDEOMEMORY") || strstr(line, "Failed to allocate"))
        __atomic_add_fetch(&dxvk_oom, 1, __ATOMIC_RELAXED);
    if (strstr(line, "Loaded L\"")) {
        if ((p = strstr(line, "Vampire.exe\" at "))) {
            size_t n = strspn(p + 16, "0123456789abcdefABCDEF");
            snprintf(exe_base, sizeof(exe_base), "0x%.*s", (int)(n < 20 ? n : 20), p + 16);
        }
        if (strstr(line, ".vtm\"")) __atomic_add_fetch(&g_vtm, 1, __ATOMIC_RELAXED);
        if (strstr(line, "binkw32.dll\"")) g_bink = 1;
        if (strstr(line, "Mss32.dll\"") || strstr(line, "mss32.dll\"")) g_miles = 1;
    }
}

static BatchRec batch_snapshot(int first)
{
    BatchRec r;
    pthread_mutex_lock(&batch_lock);
    r = first ? batch_first : batch_last;
    pthread_mutex_unlock(&batch_lock);
    return r;
}

/* Crossings per present between two records, "-" without two distinct ones. */
static const char *crossings_between(const BatchRec *a, const BatchRec *b, char *out, size_t size)
{
    if (!a->seen || !b->seen || b->present <= a->present || b->crossings < a->crossings) snprintf(out, size, "-");
    else snprintf(out, size, "%.0f", (double)(b->crossings - a->crossings) / (double)(b->present - a->present));
    return out;
}

/* 1, or 0: batching enabled and negotiated in the last record, with records replayed. */
static int batch_on(const BatchRec *r) { return r->seen && r->enabled && r->negotiated && r->records > 0; }

/* Placed CPU aliases (mesa-ps4 patch 0003) through libvulkan.prx; map32=ok once
 * one was made and none refused. */
static void alias_fields(char *out, size_t size)
{
    void *address;
    uint32_t made = 0, live = 0, failed = 0;
    uint64_t live_bytes = 0, peak_bytes = 0;
    const char *found = ntdll_symbol("pw_vulkan_alias_stats", &address);

    if (strcmp(found, "ok")) { snprintf(out, size, "map32=- alias_stats=%s", found); return; }
    ((void (*)(uint32_t *, uint32_t *, uint64_t *, uint64_t *, uint32_t *))(uintptr_t)address)(&made, &live,
                                                                                                &live_bytes,
                                                                                                &peak_bytes, &failed);
    snprintf(out, size, "map32=%s aliases=%u aliases_live=%u alias_live_kib=%llu alias_peak_kib=%llu "
             "aliases_failed=%u", made && !failed ? "ok" : failed ? "fail" : "none", made, live,
             (unsigned long long)(live_bytes >> 10), (unsigned long long)(peak_bytes >> 10), failed);
}

/* VideoOut's hardware cursor through libvulkan.prx (pw_videoout_cursor,
 * which win32u's patch 0740 calls after each present): its counters. */
static void cursor_fields(char *out, size_t size)
{
    struct pw_vulkan_cursor_stats c;
    void *address;
    char rc[16];
    const char *found = ntdll_symbol("pw_vulkan_cursor_stats", &address);

    memset(&c, 0, sizeof(c));
    c.size = sizeof(c);
    if (!strcmp(found, "ok") && ((int (*)(struct pw_vulkan_cursor_stats *))(uintptr_t)address)(&c)) found = "readfailed";
    if (strcmp(found, "ok")) {
        snprintf(out, size, "cursor_stats=%s cursor_shows=- cursor_errors=-", found);
        return;
    }
    snprintf(out, size, "cursor_stats=ok cursor_calls=%llu cursor_shows=%llu cursor_updates=%llu cursor_hides=%llu "
             "cursor_noout=%llu cursor_errors=%llu cursor_rc=%s cursor_at=%u,%u cursor_output=%u cursor_format=%u",
             (unsigned long long)c.calls, (unsigned long long)c.shows, (unsigned long long)c.updates,
             (unsigned long long)c.hides, (unsigned long long)c.noout, (unsigned long long)c.errors,
             pw_cursor_orbis_rc(c.last_rc, rc, sizeof(rc)), c.x, c.y, c.output, c.format);
}

/* The CPU a 32-bit program ran on: the translator when a thread ran on it,
 * native when wow64native.prx is loaded, else why neither. */
static const char *wow_cpu(void)
{
    uint64_t wow[PW_WOW_STAT_COUNT];
    const char *counted = wow_stats(wow);
    if (!strcmp(counted, "ok") && wow[PW_WOW_STAT_THREADS]) return "translator";
    return module_loaded("wow64native") == 1 ? "native" : strcmp(counted, "ok") ? counted : "none";
}

/* Unresolved faults: the translator's own count plus Wine's unhandled reports. */
static unsigned long long faults_unresolved_g(void)
{
    uint64_t wow[PW_WOW_STAT_COUNT];
    wow_stats(wow);
    return (unsigned long long)wow[PW_WOW_STAT_FAULTS_UNRESOLVED] +
           __atomic_load_n(&wine_unhandled, __ATOMIC_RELAXED);
}

/* A point of g's window: the time, RADV's presents and present intervals, the last batch record. */
typedef struct GSnap {
    uint64_t ns, presents;
    BatchRec batch;
    uint64_t hist[PW_VULKAN_HIST_BUCKETS];
    int hist_ok;
    uint64_t roles[ROLES];   /* the census: run time by role (main, cs, submit, other) */
    int census_ok;
} GSnap;

static void g_snap(GSnap *s, int reset_max, uint64_t *max_ns)
{
    struct pw_vulkan_stats vk;
    void *address;

    memset(s, 0, sizeof(*s));
    s->ns = cfg.now_ns();
    vulkan_stats(&vk);
    s->presents = vk.presents;
    s->batch = batch_snapshot(0);
    s->census_ok = census_roles(s->roles) == 0;
    if (!strcmp(ntdll_symbol("pw_vulkan_present_hist", &address), "ok")) {
        uint64_t max = 0;
        s->hist_ok = ((size_t (*)(uint64_t *, size_t, uint64_t *, int))(uintptr_t)address)(
                         s->hist, PW_VULKAN_HIST_BUCKETS, &max, reset_max) == PW_VULKAN_HIST_BUCKETS;
        if (max_ns) *max_ns = max;
    }
}

/* The window [a, b]: presents per second (fps_avg: RADV's counter, one per D3D9
 * Present), the frame times from the present intervals (p95 as the upper edge
 * of its 1 ms bucket; max only where the window reset it: max_ns, else "-"),
 * frames of 50 ms or more, crossings per present. */
#define G_NO_MAX UINT64_MAX
static void g_window(const GSnap *a, const GSnap *b, uint64_t max_ns, const char *prefix, char *out, size_t size)
{
    double seconds = b->ns > a->ns ? (double)(b->ns - a->ns) / 1e9 : 0.0;
    uint64_t presents = b->presents - a->presents, total = 0, cum = 0, hitches = 0;
    char crossings[24], p95[24] = "-", max[24] = "-";

    if (a->hist_ok && b->hist_ok) {
        for (size_t i = 0; i < PW_VULKAN_HIST_BUCKETS; i++) total += b->hist[i] - a->hist[i];
        for (size_t i = 0; i < PW_VULKAN_HIST_BUCKETS && total; i++) {
            uint64_t d = b->hist[i] - a->hist[i];
            if (i >= 50) hitches += d;
            cum += d;
            if (p95[0] == '-' && cum * 100 >= total * 95) snprintf(p95, sizeof(p95), "%u", (unsigned)(i + 1) * 1000u);
        }
    }
    if (max_ns != G_NO_MAX && a->hist_ok) snprintf(max, sizeof(max), "%llu", (unsigned long long)(max_ns / 1000));
    crossings_between(&a->batch, &b->batch, crossings, sizeof(crossings));
    {
        /* CPU time per frame by role (the census), "-" without it on both ends. */
        static const char *const role_key[ROLES] = { "main", "cs", "submit", "other" };
        char cpu[200] = "";
        size_t m = 0;
        for (int r = 0; r < ROLES && m < sizeof(cpu); r++) {
            if (a->census_ok && b->census_ok && presents && b->roles[r] >= a->roles[r])
                m += (size_t)snprintf(cpu + m, sizeof(cpu) - m, " %s%s_cpu_us_avg=%.0f", prefix, role_key[r],
                                      (double)(b->roles[r] - a->roles[r]) / (double)presents);
            else
                m += (size_t)snprintf(cpu + m, sizeof(cpu) - m, " %s%s_cpu_us_avg=-", prefix, role_key[r]);
        }
        snprintf(out, size, "%s", cpu);
    }
    {
    size_t used = strlen(out);
    out += used;
    size -= used;
    snprintf(out, size, " %sfps_avg=%.1f %spresents=%llu %swindow_s=%.0f %scrossings_per_frame=%s %sframe_us_avg=%.0f "
             "%sframe_us_p95=%s %sframe_us_max=%s %shitches_50ms=%s%llu", prefix,
             seconds > 0 ? (double)presents / seconds : 0.0, prefix, (unsigned long long)presents, prefix, seconds,
             prefix, crossings, prefix, presents ? seconds * 1e6 / (double)presents : 0.0, prefix, p95, prefix, max,
             prefix, total ? "" : "-", total ? (unsigned long long)hitches : 0ull);
    if (!total) {
        char *zero = strstr(out, "hitches_50ms=-0");
        if (zero) zero[14] = 0;   /* "-", not "-0" */
    }
    }
}

/* Wine's live threads now and at the most (patch 1110's pw_wine_thread_live). */
static void live_thread_fields(char *out, size_t size)
{
    void *address;
    int32_t live = -1, peak = -1;   /* Wine's LONG: 32 bits on the Unix side (L1 read 64 and printed garbage) */
    if (strcmp(ntdll_symbol("pw_wine_thread_live", &address), "ok")) {
        snprintf(out, size, "threads_live=- threads_live_peak=-");
        return;
    }
    ((void (*)(int32_t *, int32_t *))(uintptr_t)address)(&live, &peak);
    snprintf(out, size, "threads_live=%d threads_live_peak=%d", (int)live, (int)peak);
}

/* Wine's direct memory: the peaks, total and below 4 GiB (__wine_ps5_memory_stats). */
static void dmem_fields(char *out, size_t size)
{
    void *address;
    uint64_t mem[10] = { 0 };
    if (strcmp(ntdll_symbol("__wine_ps5_memory_stats", &address), "ok") ||
        ((unsigned (*)(uint64_t *, unsigned))(uintptr_t)address)(mem, 10) < 8) {
        snprintf(out, size, "dmem_peak_mib=- dmem_low_peak_mib=-");
        return;
    }
    snprintf(out, size, "dmem_peak_mib=%llu dmem_low_peak_mib=%llu", (unsigned long long)(mem[1] >> 20),
             (unsigned long long)(mem[7] >> 20));
}

/* Milestone g's own constants: the window after the gameplay mark, a segment's
 * warm-up and length, how long plan g plays. */
enum { G_WINDOW_S = 300, G_SEG_WARM_S = 10, G_SEG_S = 60, G_PLAY_S = 7200 };

/* g1n, g1m, g1: what their lines add to f1's, e2's and e's stage fields (which
 * would overflow the stage's own buffer): the CPU, the native readiness, the
 * unresolved faults (the translator's and Wine's unhandled reports), and for
 * the Vulkan programs the batching and the placed maps. */
static char g_extra[1200];

static void g1_extra_fields(char *out, size_t size)
{
    BatchRec a = batch_snapshot(1), b = batch_snapshot(0);
    char caps[16] = "-", crossings[24], alias[200] = "";
    void *caps_fn = NULL;
    size_t n;

    if (!strcmp(ntdll_symbol("__wine_prospero_native_wow64_caps", &caps_fn), "ok") && caps_fn)
        snprintf(caps, sizeof(caps), "%u", ((unsigned (*)(unsigned))(uintptr_t)caps_fn)(1));
    char live[64];
    live_thread_fields(live, sizeof(live));
    n = (size_t)snprintf(out, size, " cpu=%s native_caps=%s faults_unresolved=%llu faults_unhandled=%u %s", wow_cpu(),
                         caps, faults_unresolved_g(), __atomic_load_n(&wine_unhandled, __ATOMIC_RELAXED), live);
    if (plan->lines == g1n_lines || n >= size) return;
    alias_fields(alias, sizeof(alias));
    snprintf(out + n, size - n, " batch=%d crossings_per_frame=%s batch_lines=%llu %s", batch_on(&b),
             crossings_between(&a, &b, crossings, sizeof(crossings)), (unsigned long long)batch_lines, alias);
}

/* ---- the plans -------------------------------------------------------------------- */

static void d1_lines(void);
static void d2_lines(void);
static void d4_lines(void);
static void d_lines(void);
static void e1_lines(void);
static void e2_lines(void);
static void e_lines(void);
static void f1_lines(void);
static void f_lines(void);
static void p1_lines(void);
static void p2_lines(void);
static void p3_lines(void);
static void p4_lines(void);
static void g1n_lines(void);
static void g1m_lines(void);
static void g1_lines(void);
static void g_lines(void);
static void gc1_lines(void);
static void gc2_lines(void);

static const PwMsStage d1_stages[] = {
    { "bind_early", 10, stage_bind_early, "early_load=fault order_needed=-" },
    { "bind_lib", 10, stage_bind_lib, "lib=fault" },
    { "bind_import", 10, stage_bind_import, "import=fault" },
    { "bind_thunk", 10, stage_bind_thunk, "thunk=fault" },
    { "bind_data", 10, stage_bind_data, "data=fault" },
    { "w32u_bind", 20, stage_w32u_bind, "w32u_load=fault w32u_bound=fault" },
    { "videoout", 15, stage_videoout, "videoout=fault fb=fault" },
    { "pad", 15, stage_pad, "pad=fault pad_cross=0" },
    { "hello", 0, NULL, "hello=fault" },
};
static const PwMsStage d2_stages[] = {
    { "w32u_bind", 20, stage_w32u_bind, "w32u_load=fault w32u_bound=fault" },
    { "ft_bind", 20, stage_ft_bind, "ft_load=fault ft_bound=fault" },
    { "fonts_dir", 10, stage_fonts_dir, "fontdir=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "gdi", 0, NULL, "exit=fault" },
};
static const PwMsStage d4_stages[] = {
    { "display", 15, stage_display, "display=fault" },
    { "winemine", 0, NULL, "exit=fault" },
};
static const PwMsStage d_stages[] = {
    { "display", 15, stage_display, "display=fault" },
    { "play", 0, NULL, "exit=fault" },
};
/* e1: RADV in the title, no Wine program until the c regression. vk_caps
 * runs before the display is opened, so a device that cannot be created
 * is known without touching VideoOut. */
static const PwMsStage e1_stages[] = {
    { "vk_load", 30, stage_vk_load, "vk_load=fault vk_bound=fault" },
    { "vk_caps", 30, stage_vk_caps, "inst=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "release", 10, stage_release, "release=fault" },
    { "vk_present", 60, stage_vk_present, "present=fault gpu_hang=-" },
    { "hello", 0, NULL, "hello=fault" },
};
/* e2 and e3 from one launch of vk64.exe: winevulkan's slots before Wine
 * starts, the display open so that win32u's surface hands it over. */
static const PwMsStage e2_stages[] = {
    { "w32u_bind", 20, stage_w32u_bind, "w32u_load=fault w32u_bound=fault" },
    { "wv_bind", 20, stage_wv_bind, "wv_load=fault wv_bound=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "vk64", 0, NULL, "exit=fault" },
};
/* e: milestone e's own line, d3d11tri64.exe under DXVK 2.6.2. */
static const PwMsStage e_stages[] = {
    { "display", 15, stage_display, "display=fault" },
    { "d3d11", 0, NULL, "exit=fault" },
};
/* f1 and f2 from one launch of wow32.exe (32-bit, on the translator):
 * wowprospero's slots and the address space below 4 GiB before Wine
 * starts, the display for the GDI picture. */
static const PwMsStage f1_stages[] = {
    { "wow_bind", 20, stage_wow_bind, "wow_load=fault wow_bound=fault" },
    { "low_map", 10, stage_low_map, "low_map=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "wow32", 0, NULL, "exit=fault" },
};
/* f: milestone f's own line, Space Cadet Pinball (C:\\Pinball, the user's
 * files) on the translator, played with the DualShock 4 (f_script). */
static const PwMsStage f_stages[] = {
    { "wow_bind", 20, stage_wow_bind, "wow_load=fault wow_bound=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "pinball", 0, NULL, "exit=fault" },
};
static const PwMsStage p_stages[] = {
    { "wow_bind", 20, stage_wow_bind, "wow_load=fault wow_bound=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "pattern", 15, stage_pattern, "vo_flip=fault" },
    { "pinball", 0, NULL, "exit=fault" },
};
/* p2: native WoW64 readiness measured before Wine, then the benchmark on
 * whatever CPU the prefix gives (the translator until an Orbis native
 * backend exists). */
static const PwMsStage p2_stages[] = {
    { "wow_bind", 20, stage_wow_bind, "wow_load=fault wow_bound=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "native_probe", 15, stage_native_probe, "probe_caps=fault" },
    { "pinball", 0, NULL, "exit=fault" },
};
/* p5: Wine's audio driver into the title's sceAudioOut port (stage audio),
 * the benchmark on p4's path, then the scripted tail that plays sounds. */
static const PwMsStage p5_stages[] = {
    { "wow_bind", 20, stage_wow_bind, "wow_load=fault wow_bound=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "audio", 15, stage_audio, "audio_open=fault" },
    { "pinball", 0, NULL, "exit=fault" },
};
/* p6: libkernel's own thread stacks, without Wine or the title's pthread
 * layer: a joinable thread with a 16 MiB stack and guard 0 (the shape the
 * layer's floor gives Wine's threads), created through scePthreadCreate,
 * sampled alive, joined, twice. sp_lib_alive1_kib/sp_lib_kib1: what the
 * first took while alive and what it left behind after the join;
 * sp_lib_alive2_kib/sp_lib_kib2: the same for the second, from the first's
 * end (about 0 when libkernel reuses the stack it kept). */
static volatile int sp_alive, sp_release;

static void *stack_probe_thread(void *arg)
{
    (void)arg;
    __atomic_store_n(&sp_alive, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&sp_release, __ATOMIC_ACQUIRE)) usleep(1000);
    return NULL;
}

/* One round: the free flexible memory while the thread is alive and after
 * the join in *ALIVE and *AFTER; libkernel's result, or the join's. */
static int stack_probe_round(long long *alive, long long *after)
{
    pthread_attr_t attr;
    pthread_t thread;
    int rc, waited;

    *alive = *after = -1;
    if ((rc = pthread_attr_init(&attr))) return rc;
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_attr_setguardsize(&attr, 0);
    sp_alive = sp_release = 0;
    rc = scePthreadCreate(&thread, &attr, stack_probe_thread, NULL, "stack_probe");
    pthread_attr_destroy(&attr);
    if (rc) return rc;
    for (waited = 0; !__atomic_load_n(&sp_alive, __ATOMIC_ACQUIRE) && waited < 2000; waited++) usleep(1000);
    *alive = flex_free();
    __atomic_store_n(&sp_release, 1, __ATOMIC_RELEASE);
    rc = pthread_join(thread, NULL);
    *after = flex_free();
    return rc;
}

static void stage_stack_probe(char *fields, size_t size)
{
    long long start = flex_free(), alive1, after1, alive2 = -1, after2 = -1;
    int rc1 = stack_probe_round(&alive1, &after1), rc2 = -1;
    char a1[24] = "-", k1[24] = "-", a2[24] = "-", k2[24] = "-";

    if (!rc1) rc2 = stack_probe_round(&alive2, &after2);
    if (start >= 0 && alive1 >= 0) snprintf(a1, sizeof(a1), "%lld", (start - alive1) / 1024);
    if (start >= 0 && after1 >= 0) snprintf(k1, sizeof(k1), "%lld", (start - after1) / 1024);
    if (after1 >= 0 && alive2 >= 0) snprintf(a2, sizeof(a2), "%lld", (after1 - alive2) / 1024);
    if (after1 >= 0 && after2 >= 0) snprintf(k2, sizeof(k2), "%lld", (after1 - after2) / 1024);
    snprintf(fields, size, "sp_lib_rc=%#x,%#x sp_lib_alive1_kib=%s sp_lib_kib1=%s sp_lib_alive2_kib=%s sp_lib_kib2=%s",
             (unsigned)rc1, (unsigned)rc2, a1, k1, a2, k2);
    step("stack_probe: %s", fields);
}

/* p5w: wincb.exe's window cases on the main thread and on new threads, on
 * p5's stages without the audio (a 32-bit program: wowprospero's slots). */
static const PwMsStage p5w_stages[] = {
    { "wow_bind", 20, stage_wow_bind, "wow_load=fault wow_bound=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "wincb", 0, NULL, "exit=fault" },
};
/* p6: the thread stacks, libkernel's own stacks first (stack_probe), then
 * p5w's wincb.exe with its two holds, under WINE_PS5_THREAD_STACK. */
static const PwMsStage p6_stages[] = {
    { "stack_probe", 15, stage_stack_probe, "stack_probe=fault" },
    { "wow_bind", 20, stage_wow_bind, "wow_load=fault wow_bound=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "wincb", 0, NULL, "exit=fault" },
};
/* Milestone g (docs/ps4/HANDOFF-g.md in ps4-wine). g1n: f1's stages, wow32.exe on the native
 * CPU; g1m: e2's, vk32.exe; g1: the display, d3d9tri32.exe under DXVK's 32-bit d3d9; g:
 * wowprospero's slots (the translator is the fallback CPU), the display, Bloodlines. */
static const PwMsStage g1n_stages[] = {
    { "cpu_probe", 10, stage_cpu_probe, "cpu_probe=fault" },
    { "wow_bind", 20, stage_wow_bind, "wow_load=fault wow_bound=fault" },
    { "low_map", 10, stage_low_map, "low_map=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "wow32", 0, NULL, "exit=fault" },
};
static const PwMsStage g1m_stages[] = {
    { "cpu_probe", 10, stage_cpu_probe, "cpu_probe=fault" },
    { "w32u_bind", 20, stage_w32u_bind, "w32u_load=fault w32u_bound=fault" },
    { "wv_bind", 20, stage_wv_bind, "wv_load=fault wv_bound=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "vk32", 0, NULL, "exit=fault" },
};
static const PwMsStage g1_stages[] = {
    { "cpu_probe", 10, stage_cpu_probe, "cpu_probe=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "d3d9", 0, NULL, "exit=fault" },
};
static const PwMsStage g_stages[] = {
    { "cpu_probe", 10, stage_cpu_probe, "cpu_probe=fault" },
    { "wow_bind", 20, stage_wow_bind, "wow_load=fault wow_bound=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "audio", 15, stage_audio, "audio_open=fault" },
    { "vtmb", 0, NULL, "exit=fault" },
};
/* g's input step, VideoOut's hardware cursor: gc1 the four calls on the
 * title's own display (no Wine, no RADV), then hello.exe; gc2 e1's stages
 * with libvulkan.prx's pw_videoout_cursor called after every present. */
static const PwMsStage gc1_stages[] = {
    { "cursor", 90, stage_cursor, "cursor=fault mem=fault" },
    { "hello", 0, NULL, "hello=fault" },
};
static const PwMsStage gc2_stages[] = {
    { "vk_load", 30, stage_vk_load, "vk_load=fault vk_bound=fault" },
    { "vk_caps", 30, stage_vk_caps, "inst=fault" },
    { "display", 15, stage_display, "display=fault" },
    { "release", 10, stage_release, "release=fault" },
    { "vk_cursor", 90, stage_vk_cursor, "present=fault gpu_hang=- cursor_export=fault" },
    { "hello", 0, NULL, "hello=fault" },
};
static const PwMsPlan plans[] = {
    { "d1", d1_stages, sizeof(d1_stages) / sizeof(d1_stages[0]), "C:\\hello.exe", 1, d1_lines },
    { "d2", d2_stages, sizeof(d2_stages) / sizeof(d2_stages[0]), "C:\\gdi64.exe", 0, d2_lines },
    { "d4", d4_stages, sizeof(d4_stages) / sizeof(d4_stages[0]), "C:\\windows\\system32\\winemine.exe", 0, d4_lines },
    { "d", d_stages, sizeof(d_stages) / sizeof(d_stages[0]), "C:\\windows\\system32\\winemine.exe", 0, d_lines },
    { "e1", e1_stages, sizeof(e1_stages) / sizeof(e1_stages[0]), "C:\\hello.exe", 1, e1_lines },
    { "e2", e2_stages, sizeof(e2_stages) / sizeof(e2_stages[0]), "C:\\vk64.exe", 0, e2_lines },
    { "e", e_stages, sizeof(e_stages) / sizeof(e_stages[0]), "C:\\d3d11tri64.exe", 0, e_lines },
    { "f1", f1_stages, sizeof(f1_stages) / sizeof(f1_stages[0]), "C:\\wow32.exe", 0, f1_lines },
    { "f", f_stages, sizeof(f_stages) / sizeof(f_stages[0]), "C:\\Pinball\\PINBALL.EXE", 0, f_lines },
    /* p1: plan f's Pinball as the benchmark (p_script): the segment's measurements. */
    { "p1", f_stages, sizeof(f_stages) / sizeof(f_stages[0]), "C:\\Pinball\\PINBALL.EXE", 0, p1_lines },
    { "p2", p2_stages, sizeof(p2_stages) / sizeof(p2_stages[0]), "C:\\Pinball\\PINBALL.EXE", 0, p2_lines },
    /* p3, p4: the benchmark on another present path, after the HDMI test pattern. */
    { "p3", p_stages, sizeof(p_stages) / sizeof(p_stages[0]), "C:\\Pinball\\PINBALL.EXE", 0, p3_lines },
    { "p4", p_stages, sizeof(p_stages) / sizeof(p_stages[0]), "C:\\Pinball\\PINBALL.EXE", 0, p4_lines },
    /* Milestone g: g's program comes from its profile (<root>/profiles/vtmb.ini). */
    { "g1n", g1n_stages, sizeof(g1n_stages) / sizeof(g1n_stages[0]), "C:\\wow32.exe", 0, g1n_lines },
    { "g1m", g1m_stages, sizeof(g1m_stages) / sizeof(g1m_stages[0]), "C:\\vk32.exe", 0, g1m_lines },
    { "g1", g1_stages, sizeof(g1_stages) / sizeof(g1_stages[0]), "C:\\d3d9tri32.exe", 0, g1_lines },
    { "g", g_stages, sizeof(g_stages) / sizeof(g_stages[0]), "C:\\VTMB\\Vampire.exe", 0, g_lines },
    { "gc1", gc1_stages, sizeof(gc1_stages) / sizeof(gc1_stages[0]), "C:\\hello.exe", 1, gc1_lines },
    { "gc2", gc2_stages, sizeof(gc2_stages) / sizeof(gc2_stages[0]), "C:\\hello.exe", 1, gc2_lines },
    /* p6 and p5w before p5: the last plan is the default. */
    { "p6", p6_stages, sizeof(p6_stages) / sizeof(p6_stages[0]), "C:\\wincb32.exe", 0, p6_lines },
    { "p5w", p5w_stages, sizeof(p5w_stages) / sizeof(p5w_stages[0]), "C:\\wincb32.exe", 0, p5w_lines },
    { "p5", p5_stages, sizeof(p5_stages) / sizeof(p5_stages[0]), "C:\\Pinball\\PINBALL.EXE", 0, p5_lines },
};

static int g1_plans(void)
{
    return plan && (plan->lines == g1n_lines || plan->lines == g1m_lines || plan->lines == g1_lines);
}
static int g_plan(void) { return plan && plan->lines == g_lines; }
static int vulkan_plan(void)
{
    return plan && (plan->lines == e2_lines || plan->lines == e_lines || plan->lines == g1m_lines ||
                    plan->lines == g1_lines || g_plan());
}
const char *pw_ms_profile_id(void) { return g_plan() ? "vtmb" : NULL; }
static int dxvk_plan(void) { return plan && plan->lines == e_lines; }
static int p_plan(void)
{
    return plan && (plan->lines == p1_lines || plan->lines == p2_lines || plan->lines == p3_lines ||
                    plan->lines == p4_lines || plan->lines == p5_lines);
}
static int pinball_plan(void) { return plan && (plan->lines == f_lines || p_plan()); }
static int native_plan(void) { return plan && plan->lines == p2_lines; }
static int wincb_plan(void) { return plan && (plan->lines == p5w_lines || plan->lines == p6_lines); }

/* The Wine stage's watchdog (wine64_main_orbis.c), 0 for its default of
 * 180 s: f plays up to F_PLAY_S, and its play loop sends the line itself
 * (the first f launch's line came from the default watchdog at 180 s, in
 * the middle of the play, with play_s=0). */
unsigned pw_ms_wine_watchdog_s(void)
{
    /* g: a 300 s window after the user reaches the first map, then segments; h reuses it. */
    return pinball_plan() ? F_PLAY_S + 60 : g_plan() ? G_PLAY_S + 60 : 0;
}

const char *pw_ms_select(const char *name)
{
    plan = &plans[sizeof(plans) / sizeof(plans[0]) - 1];   /* the newest milestone */
    for (size_t i = 0; name && i < sizeof(plans) / sizeof(plans[0]); i++)
        if (!strcmp(plans[i].name, name)) plan = &plans[i];
    bench_plan = p_plan();
    return plan->name;
}

const char *pw_ms_exe(void) { return plan->exe; }
int pw_ms_c_line(void) { return plan->c_line; }

/* ---- faults and the watchdog ----------------------------------------------- */

static unsigned wine_stage(void) { return plan->count - 1; }

static void stage_fault(int sig, siginfo_t *info, void *context)
{
    uint64_t rip = 0;
    int stage = stage_active;
    char detail[200], where[120];

    memcpy(&rip, (const uint8_t *)context + 224, sizeof(rip));
    snprintf(detail, sizeof(detail), "sig=%d addr=0x%llx rip=%s", sig,
             (unsigned long long)(uintptr_t)(info ? info->si_addr : NULL), pw_orbis_module_of(rip, where, sizeof(where)));
    if (stage < 0 || (unsigned)stage >= wine_stage() || recovering) {
        /* Outside a stage, or a fault while recovering: record it, leave the
         * stage begun (the next launch skips it), say what is known, idle. */
        if (stage >= 0) pw_stage_mark(&run, (unsigned)stage, 'F', detail);
        else cfg.emit(detail);
        pw_ms_emit_lines();
        for (;;) sleep(60);
    }
    recovering = 1;
    pw_stage_mark(&run, (unsigned)stage, 'F', detail);
    longjmp(stage_jump, 1);
}

static void set_handlers(void (*handler)(int, siginfo_t *, void *))
{
    static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE };
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    if (handler) {
        action.sa_sigaction = handler;
        action.sa_flags = SA_SIGINFO | SA_NODEFER;
    } else {
        action.sa_handler = SIG_DFL;
    }
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) sigaction(signals[i], &action, NULL);
}

static void *watchdog(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec pause = { 0, 200000000 };
        int stage = stage_active;
        uint64_t deadline = stage_deadline;
        nanosleep(&pause, NULL);
        if (stage >= 0 && deadline && stage == stage_active && cfg.now_ns() > deadline) {
            char detail[32];
            snprintf(detail, sizeof(detail), "limit=%us", plan->stages[stage].limit_s);
            pw_stage_mark(&run, (unsigned)stage, 'H', detail);
            pw_ms_emit_lines();
            return NULL;   /* the stage's thread cannot be stopped; the title idles in it */
        }
    }
}

/* ---- the run -------------------------------------------------------------------- */

void pw_ms_start(const PwMsConfig *config)
{
    static const char *names[PW_STAGE_MAX];
    char dir[256];
    pthread_t thread;

    if (!plan) pw_ms_select(NULL);
    cfg = *config;
    if (bench_plan) {
        /* The plan's defaults (p3: vo720; p4: vo720 and async), then env.txt's. */
        char v[32];
        /* p5 runs on p4's path (p4 passed: vo720, async), as its rate gate assumes. */
        p_path = plan->lines == p3_lines || plan->lines == p4_lines || plan->lines == p5_lines ? P_PATH_VO720
                                                                                                : P_PATH_SCALE;
        p_async = plan->lines == p4_lines || plan->lines == p5_lines;
        if (plan->lines == p5_lines) {
            p_audio = 1;
            if (cfg.setting && cfg.setting("PS4WINE_AUDIO", v, sizeof(v)))
                p_audio = !strcmp(v, "off") ? 0 : 1;
        }
        if (cfg.setting && cfg.setting("PS4WINE_PRESENT_PATH", v, sizeof(v)))
            for (int i = 0; i < 3; i++)
                if (!strcmp(v, p_path_names[i])) p_path = i;
        if (cfg.setting && cfg.setting("PS4WINE_PRESENT_MODE", v, sizeof(v)))
            p_async = !strcmp(v, "async") ? 1 : !strcmp(v, "sync") ? 0 : p_async;
    }
    if (plan->lines == g_lines) {
        /* g plays with sound through the port p5 measured (stage_audio, Wine's ps5 audio driver);
         * PS4WINE_AUDIO=off in env.txt turns it off. */
        char v[32];
        p_audio = 1;
        if (cfg.setting && cfg.setting("PS4WINE_AUDIO", v, sizeof(v))) p_audio = !strcmp(v, "off") ? 0 : 1;
    }
    main_thread = pthread_self();
    start_ns = cfg.now_ns();
    snprintf(dir, sizeof(dir), "%s/%s", cfg.root, plan->name);
    mkdir(dir, 0777);
    snprintf(progress_path, sizeof(progress_path), "%s/progress.txt", dir);
    for (unsigned i = 0; i < plan->count && i < PW_STAGE_MAX; i++) names[i] = plan->stages[i].name;
    pw_stage_init(&run, &stage_ops, progress_path, "PW_MS", names, plan->count);
    if (pw_stage_load(&run)) cfg.emit("PW_MS progress file not writable; results of this launch are not kept");
    {
        char summary[512], line[640];
        pw_stage_summary(&run, summary, sizeof(summary));
        snprintf(line, sizeof(line), "PW_MS start plan=%s exe=%s %s file=%s", plan->name, plan->exe, summary,
                 progress_path);
        cfg.emit(line);
    }
    set_handlers(stage_fault);
    pthread_create(&thread, NULL, watchdog, NULL);
}

void pw_ms_title_stages(void)
{
    static char fields[PW_STAGE_FIELDS];

    for (unsigned i = 0; i < wine_stage(); i++) {
        if (!pw_stage_should_run(&run, i)) continue;
        pw_stage_begin(&run, i);
        fields[0] = 0;
        recovering = 0;
        stage_deadline = cfg.now_ns() + (uint64_t)plan->stages[i].limit_s * 1000000000ull;
        stage_active = (int)i;
        if (setjmp(stage_jump) == 0) plan->stages[i].run(fields, sizeof(fields));
        else snprintf(fields, sizeof(fields), "%s", plan->stages[i].fault_fields);
        stage_active = -1;
        stage_deadline = 0;
        pw_stage_end(&run, i, fields);
    }
}

int pw_ms_wine_should_run(void) { return pw_stage_should_run(&run, wine_stage()); }

void pw_ms_wine_begin(void)
{
    flex_start = flex_free();
    set_handlers(NULL);
    pw_stage_begin(&run, wine_stage());
    if (g_plan()) {
        pthread_t thread;
        if (!pthread_create(&thread, NULL, g_script, NULL)) pthread_detach(thread);
    }
    if (plan->lines == d4_lines || plan->lines == d_lines || plan->lines == f_lines || p_plan()) {
        pthread_t thread;
        if (!pthread_create(&thread, NULL, plan->lines == d4_lines ? d4_script : plan->lines == d_lines ? d_script
                            : p_plan() ? p_script : f_script, NULL))
            pthread_detach(thread);
    }
}

void pw_ms_wine_end(const char *exit_text, const char *last_stdout, int server_connected)
{
    char fields[PW_STAGE_FIELDS];

    if (plan->lines == d_lines) {
        save_last_drawn();
        pthread_mutex_lock(&present_lock);
        snprintf(fields, sizeof(fields), "exit=%s frames=%llu inputs=%d refused=%d board_opened=%d fb=%s drawn=%llu "
                 "pad=%s pad_reads=%d end=%s probe_before=%06x probe_after=%06x lat_avg_ms=%llu lat_max_ms=%llu "
                 "present_ms=%.1f", exit_text,
                 (unsigned long long)frames, d_inputs, d4_refused, d4_board_opened < 0 ? 0 : d4_board_opened,
                 fb_saved == 0 ? "ok" : fb_saved == -1 ? "none" : "fail", (unsigned long long)drawn,
                 d_pad == 1 ? "ok" : d_pad == 0 ? "none" : "-", d_pad_reads, d_end, (unsigned)d4_before,
                 (unsigned)d4_after, (unsigned long long)(lat_n ? lat_sum_ns / lat_n / 1000000ull : 0),
                 (unsigned long long)(lat_max_ns / 1000000ull),
                 present_n ? present_sum_ns / (double)present_n / 1e6 : 0.0);
        pthread_mutex_unlock(&present_lock);
    } else if (plan->lines == d4_lines) {
        save_last_drawn();
        pthread_mutex_lock(&present_lock);
        snprintf(fields, sizeof(fields), "exit=%s frames=%llu drawn=%llu frame=%ux%u fb=%s board_opened=%d "
                 "probe_before=%06x probe_after=%06x refused=%d posted=%d", exit_text, (unsigned long long)frames,
                 (unsigned long long)drawn, frame_width, frame_height,
                 fb_saved == 0 ? "ok" : fb_saved == -1 ? "none" : "fail", d4_board_opened < 0 ? 0 : d4_board_opened,
                 (unsigned)d4_before, (unsigned)d4_after, d4_refused, d4_posted);
        pthread_mutex_unlock(&present_lock);
    } else if (plan->lines == e2_lines || plan->lines == g1m_lines) {
        /* vk64.exe's two lines, its frames and hang renamed app_*: frames, the
         * swapchain and the lost-device count are what reached the driver. */
        static const char *const rename[] = { "frames", "app_frames", "swapchain", "app_swapchain", "hang", "app_hang" };
        struct pw_vulkan_stats vk;
        char shot[512], second[300];
        struct stat st;
        const char *counted = vulkan_stats(&vk);
        int fb;
        snprintf(shot, sizeof(shot), "%s/shot.raw", cfg.root);
        fb = stat(shot, &st) == 0 && st.st_size == (off_t)PW_VIDEOOUT_ORBIS_WIDTH * PW_VIDEOOUT_ORBIS_HEIGHT * 4;
        renamed(vk64s_line[0] ? vk64s_line : "surface=-", rename, 3, second, sizeof(second));
        snprintf(fields, sizeof(fields), "exit=%s %s %s frames=%llu swapchain=%ux%u gpu_hang=%d device_lost=%u fb=%s "
                 "release=%s gdi_dropped=%llu vk_stats=%s", exit_text, vk64_line[0] ? vk64_line : "inst=-", second,
                 (unsigned long long)vk.presents, vk.width, vk.height,
                 strstr(vk64s_line, "hang=1") != NULL || vk.device_lost != 0, vk.device_lost, fb ? "ok" : "none",
                 release_state, (unsigned long long)gdi_dropped, counted);
    } else if (plan->lines == e_lines || plan->lines == g1_lines) {
        /* d3d11tri64.exe's line, its frames and swapchain renamed app_*: frames,
         * the swapchain and lost devices are what reached the driver; a hang is
         * a lost device or DXGI's removed/hung/reset device. */
        static const char *const rename[] = { "frames", "app_frames", "swapchain", "app_swapchain" };
        struct pw_vulkan_stats vk;
        char shot[512], line[300];
        struct stat st;
        const char *counted = vulkan_stats(&vk);
        int fb, removed;
        snprintf(shot, sizeof(shot), "%s/shot.raw", cfg.root);
        fb = stat(shot, &st) == 0 && st.st_size == (off_t)PW_VIDEOOUT_ORBIS_WIDTH * PW_VIDEOOUT_ORBIS_HEIGHT * 4;
        removed = strstr(d3d11_line, "hr=0x887a0005") || strstr(d3d11_line, "hr=0x887a0006") ||
                  strstr(d3d11_line, "hr=0x887a0007");
        renamed(d3d11_line[0] ? d3d11_line : "app_frames=-", rename, 2, line, sizeof(line));
        snprintf(fields, sizeof(fields), "exit=%s %s frames=%llu swapchain=%ux%u gpu_hang=%d device_lost=%u fb=%s "
                 "release=%s gdi_dropped=%llu vk_stats=%s", exit_text, line, (unsigned long long)vk.presents, vk.width,
                 vk.height, removed || vk.device_lost != 0, vk.device_lost, fb ? "ok" : "none", release_state,
                 (unsigned long long)gdi_dropped, counted);
        {
            /* The thread stacks (p6): ntdll's layer, then libvulkan's own copy
             * (the floor RADV's threads got). */
            char threads[400], vk_threads[160];
            size_t n = strlen(fields);
            thread_fields(threads, sizeof(threads));
            vk_thread_fields(vk_threads, sizeof(vk_threads));
            snprintf(fields + n, sizeof(fields) - n, " %s %s", threads, vk_threads);
        }
    } else if (p_plan()) {
        /* The benchmark: the translator's counters and the display here, the
         * segment's measurements kept aside for the line (they do not fit
         * the stage's fields). */
        uint64_t wow[PW_WOW_STAT_COUNT];
        const char *counted = wow_stats(wow);
        char caps[16] = "-";
        void *caps_fn = NULL;
        /* Patch 0883's readiness export, when the ntdll has one. */
        if (!strcmp(ntdll_symbol("__wine_prospero_native_wow64_caps", &caps_fn), "ok") && caps_fn)
            snprintf(caps, sizeof(caps), "%u", ((unsigned (*)(unsigned))(uintptr_t)caps_fn)(1));
        save_last_drawn();
        pthread_mutex_lock(&present_lock);
        snprintf(fields, sizeof(fields), "exit=%s native_caps=%s cpu=%s wow_stats=%s faults_unresolved=%llu "
                 "faults_guest=%llu faults_marked=%llu faults_host=%llu faults_smc=%llu dbt_errors=%llu frames=%llu "
                 "pad=%s fb=%s frame=%ux%u", exit_text, caps,
                 /* native: no thread on the translator (or no translator loaded through
                  * Wine at all: its counters answer nosym) and wow64native.prx loaded */
                 !strcmp(counted, "ok") && wow[PW_WOW_STAT_THREADS] ? "translator"
                 : module_loaded("wow64native") == 1 ? "native" : strcmp(counted, "ok") ? counted : "none", counted,
                 (unsigned long long)wow[PW_WOW_STAT_FAULTS_UNRESOLVED], (unsigned long long)wow[PW_WOW_STAT_FAULTS_GUEST],
                 (unsigned long long)wow[PW_WOW_STAT_FAULTS_MARKED], (unsigned long long)wow[PW_WOW_STAT_FAULTS_HOST],
                 (unsigned long long)wow[PW_WOW_STAT_FAULTS_SMC], (unsigned long long)wow[PW_WOW_STAT_DBT_ERRORS],
                 (unsigned long long)frames, d_pad == 1 ? "ok" : d_pad == 0 ? "none" : "-",
                 fb_saved == 0 ? "ok" : fb_saved == -1 ? "none" : "fail", frame_width, frame_height);
        p_bench_fields(p_bench, sizeof(p_bench));
        pthread_mutex_unlock(&present_lock);
        {
            /* The thread stacks' cost (phase p5): appended, outside the lock. */
            char threads[400];
            size_t n = strlen(fields);
            thread_fields(threads, sizeof(threads));
            snprintf(fields + n, sizeof(fields) - n, " %s", threads);
        }
    } else if (plan->lines == f_lines) {
        /* Pinball: the translator's counters, the frames (all, and the play's
         * rate), the pad. native_caps comes with f4. */
        uint64_t wow[PW_WOW_STAT_COUNT];
        const char *counted = wow_stats(wow);
        double play_s = f_play_ns / 1e9;
        save_last_drawn();
        pthread_mutex_lock(&present_lock);
        snprintf(fields, sizeof(fields), "exit=%s cpu=%s wow_stats=%s faults_unresolved=%llu faults_guest=%llu "
                 "faults_marked=%llu faults_host=%llu faults_smc=%llu dbt_errors=%llu frames=%llu fps=%.1f "
                 "play_s=%.0f inputs=%d keys=%d refused=%d pad=%s pad_reads=%d end=%s preset=%s fb=%s frame=%ux%u "
                 "present_ms=%.1f native_caps=-", exit_text,
                 strcmp(counted, "ok") ? counted : wow[PW_WOW_STAT_THREADS] ? "translator" : "none", counted,
                 (unsigned long long)wow[PW_WOW_STAT_FAULTS_UNRESOLVED], (unsigned long long)wow[PW_WOW_STAT_FAULTS_GUEST],
                 (unsigned long long)wow[PW_WOW_STAT_FAULTS_MARKED], (unsigned long long)wow[PW_WOW_STAT_FAULTS_HOST],
                 (unsigned long long)wow[PW_WOW_STAT_FAULTS_SMC], (unsigned long long)wow[PW_WOW_STAT_DBT_ERRORS],
                 (unsigned long long)frames, play_s > 0 ? f_frames_play / play_s : 0.0, play_s, d_inputs, f_keys,
                 d4_refused, d_pad == 1 ? "ok" : d_pad == 0 ? "none" : "-", d_pad_reads, d_end, f_preset_from,
                 fb_saved == 0 ? "ok" : fb_saved == -1 ? "none" : "fail", frame_width, frame_height,
                 present_n ? present_sum_ns / (double)present_n / 1e6 : 0.0);
        pthread_mutex_unlock(&present_lock);
    } else if (plan->lines == f1_lines || plan->lines == g1n_lines) {
        /* wow32.exe: the translator's counters, HELLO32/HELLO32S's checks,
         * then GDI32's picture with the sink's counts. cpu=translator once a
         * thread ran on wowprospero. */
        uint64_t wow[PW_WOW_STAT_COUNT];
        const char *counted = wow_stats(wow);
        char fp[40], thread[40], seh[40], screen[24], pixel[16], window[32], fonts[12];
        save_last_drawn();
        pthread_mutex_lock(&present_lock);
        snprintf(fields, sizeof(fields), "exit=%s server=%s cpu=%s wow_stats=%s wow_threads=%llu faults_unresolved=%llu "
                 "faults_guest=%llu faults_marked=%llu faults_host=%llu faults_smc=%llu dbt_errors=%llu fp=%s "
                 "thread=%s seh=%s hello=%s frames=%llu drawn=%llu frame=%ux%u fb=%s screen=%s pixel=%s window=%s "
                 "fonts=%s", exit_text, server_connected ? "inprocess" : "none",
                 strcmp(counted, "ok") ? counted : wow[PW_WOW_STAT_THREADS] ? "translator" : "none", counted,
                 (unsigned long long)wow[PW_WOW_STAT_THREADS], (unsigned long long)wow[PW_WOW_STAT_FAULTS_UNRESOLVED],
                 (unsigned long long)wow[PW_WOW_STAT_FAULTS_GUEST], (unsigned long long)wow[PW_WOW_STAT_FAULTS_MARKED],
                 (unsigned long long)wow[PW_WOW_STAT_FAULTS_HOST], (unsigned long long)wow[PW_WOW_STAT_FAULTS_SMC],
                 (unsigned long long)wow[PW_WOW_STAT_DBT_ERRORS], pick(hello32_line, "fp", fp, sizeof(fp)),
                 pick(hello32_line, "thread", thread, sizeof(thread)), pick(hello32s_line, "seh", seh, sizeof(seh)),
                 hello32_done ? "ok" : "-", (unsigned long long)frames, (unsigned long long)drawn, frame_width,
                 frame_height, fb_saved == 0 ? "ok" : fb_saved == -1 ? "none" : "fail",
                 pick(gdi32_line, "screen", screen, sizeof(screen)), pick(gdi32_line, "pixel", pixel, sizeof(pixel)),
                 pick(gdi32_line, "window", window, sizeof(window)), pick(gdi32_line, "fonts", fonts, sizeof(fonts)));
        pthread_mutex_unlock(&present_lock);
    } else if (g_plan()) {
        /* Bloodlines: its exit here; the line's fields are g's window (g_lines). */
        snprintf(fields, sizeof(fields), "exit=%s server=%s", exit_text, server_connected ? "inprocess" : "none");
    } else if (wincb_plan()) {
        /* wincb.exe (p5w, p6): its summary (its "ok" renamed cases_ok), the
         * hold probe's samples and the thread stacks' fields; cpu as in the p
         * plans. */
        static const char *const rename[] = { "ok", "cases_ok" };
        uint64_t wow[PW_WOW_STAT_COUNT];
        const char *counted = wow_stats(wow);
        char done[400], hold[200], threads[400];
        renamed(wincb_done[0] ? wincb_done : "cases=-", rename, 1, done, sizeof(done));
        hold_fields(hold, sizeof(hold));
        thread_fields(threads, sizeof(threads));
        snprintf(fields, sizeof(fields), "exit=%s cpu=%s faults_unresolved=%llu %s %s %s", exit_text,
                 !strcmp(counted, "ok") && wow[PW_WOW_STAT_THREADS] ? "translator"
                 : module_loaded("wow64native") == 1 ? "native" : strcmp(counted, "ok") ? counted : "none",
                 (unsigned long long)wow[PW_WOW_STAT_FAULTS_UNRESOLVED], done, hold, threads);
    } else if (plan->c_line) {
        /* hello.exe: milestone c's regression (d1, e1). */
        int ok = !strcmp(exit_text, "0") && server_connected && !strcmp(last_stdout, "Hello from PE64");
        snprintf(fields, sizeof(fields), "hello=%s", ok ? "ok" : exit_text);
    } else {
        /* gdi64.exe's "GDI64 k=v ..." line, as it came, after the exit and the sink's counts. */
        const char *gdi = !strncmp(last_stdout, "GDI64 ", 6) ? last_stdout + 6 : "pixel=-";
        save_last_drawn();
        pthread_mutex_lock(&present_lock);
        snprintf(fields, sizeof(fields), "exit=%s server=%s frames=%llu drawn=%llu frame=%ux%u fb=%s %s", exit_text,
                 server_connected ? "inprocess" : "none", (unsigned long long)frames, (unsigned long long)drawn,
                 frame_width, frame_height, fb_saved == 0 ? "ok" : fb_saved == -1 ? "none" : "fail", gdi);
        pthread_mutex_unlock(&present_lock);
    }
    if (g1_plans()) g1_extra_fields(g_extra, sizeof(g_extra));
    if (vulkan_plan()) {
        /* Recorded, not in the line: VideoOut's hardware cursor (win32u's patch 0740). */
        char cursor[400];
        cursor_fields(cursor, sizeof(cursor));
        step("cursor at the end: %s", cursor);
    }
    pw_stage_end(&run, wine_stage(), fields);
    pw_ms_emit_lines();
}

/* A key's value: from its stage's fields if the stage ended, "crash" or
 * "hang" if it was lost, "-" if not reached. */
static const char *value(unsigned stage, const char *key, char *out, size_t size)
{
    switch (run.state[stage]) {
    case PW_STAGE_DONE: return pw_stage_value(&run, stage, key, out, size) ? out : "-";
    case PW_STAGE_CRASHED: return "crash";
    case PW_STAGE_HUNG: return "hang";
    default: return "-";
    }
}

typedef struct LineKey { const char *key; unsigned stage; } LineKey;

/* "<head> k=v ... build=... <summary> session_s=..." through cfg.emit. */
/* The line: head, the keys' values, extra (fields of its own, or NULL),
 * then build, the stage summary and session_s. */
static void emit_line_with(const char *head, const LineKey *keys, size_t count, const char *extra)
{
    char line[2048], summary[512], v[96];
    size_t n = (size_t)snprintf(line, sizeof(line), "%s", head);

    for (size_t i = 0; i < count && n < sizeof(line); i++)
        n += (size_t)snprintf(line + n, sizeof(line) - n, " %s=%s", keys[i].key,
                              value(keys[i].stage, keys[i].key, v, sizeof(v)));
    if (extra && n < sizeof(line)) n += (size_t)snprintf(line + n, sizeof(line) - n, "%s", extra);
    pw_stage_summary(&run, summary, sizeof(summary));
    if (n < sizeof(line))
        snprintf(line + n, sizeof(line) - n, " build=%s %s session_s=%llu", cfg.build_id, summary,
                 (unsigned long long)((cfg.now_ns() - (session_base_ns ? session_base_ns : start_ns)) / 1000000000ull));
    cfg.emit(line);
}

static void emit_line(const char *head, const LineKey *keys, size_t count)
{
    emit_line_with(head, keys, count, NULL);
}

static void d1_lines(void)
{
    enum { EARLY, LIB, IMPORT, THUNK, DATA, W32U, VIDEO, PAD, HELLO };
    static const LineKey keys[] = {
        { "lib", LIB }, { "import", IMPORT }, { "import_slots", IMPORT }, { "order_needed", EARLY },
        { "data", DATA }, { "thunk", THUNK }, { "w32u_load", W32U }, { "w32u_bound", W32U }, { "w32u_zero", W32U },
        { "videoout", VIDEO }, { "fb", VIDEO }, { "flip_hz", VIDEO }, { "pad", PAD }, { "pad_cross", PAD },
        { "pad_bytes", PAD }, { "hello", HELLO },
    };
    char head[64], v[96];
    const char *bind = "none";

    if (!strcmp(value(IMPORT, "import", v, sizeof(v)), "ok")) bind = "import";
    else if (!strcmp(value(THUNK, "thunk", v, sizeof(v)), "ok")) bind = "thunk";
    snprintf(head, sizeof(head), "PS4WINE d1 bind=%s", bind);
    emit_line(head, keys, sizeof(keys) / sizeof(keys[0]));
}

/* d2 and d3 from one launch of gdi64.exe: win32u inside Wine, and FreeType
 * with Wine's fonts. */
static void d2_lines(void)
{
    enum { W32U, FT, FONTDIR, DISPLAY, GDI };
    static const LineKey d2_keys[] = {
        { "pixel", GDI }, { "exit", GDI }, { "screen", GDI }, { "window", GDI }, { "frames", GDI },
        { "drawn", GDI }, { "frame", GDI }, { "fb", GDI }, { "server", GDI }, { "display", DISPLAY },
        { "w32u_bound", W32U }, { "w32u_dlsym", W32U }, { "w32u_first", W32U },
    };
    static const LineKey d3_keys[] = {
        { "tahoma", GDI }, { "text_px", GDI }, { "exit", GDI }, { "ft_bound", FT }, { "ft_first", FT },
        { "ft_init_sym", FT }, { "fontdir", FONTDIR }, { "fontdir_entries", FONTDIR }, { "fontdir_ttf", FONTDIR },
        { "fontdir_errno", FONTDIR }, { "tahoma_mmap", FONTDIR },
    };
    char head[160], pixel[96], screen[96], fonts[96];

    value(GDI, "pixel", pixel, sizeof(pixel));
    value(GDI, "screen", screen, sizeof(screen));
    /* win32u=ok: the program got as far as drawing and reading back a pixel. */
    snprintf(head, sizeof(head), "PS4WINE d2 gdi win32u=%s driver=%s",
             strcmp(pixel, "-") && strcmp(pixel, "crash") && strcmp(pixel, "hang") ? "ok" : "fail",
             !strcmp(screen, "800x600") ? "ps5" : screen);
    emit_line(head, d2_keys, sizeof(d2_keys) / sizeof(d2_keys[0]));
    snprintf(head, sizeof(head), "PS4WINE d3 fonts n=%s", value(GDI, "fonts", fonts, sizeof(fonts)));
    emit_line(head, d3_keys, sizeof(d3_keys) / sizeof(d3_keys[0]));
}

static void d4_lines(void)
{
    enum { DISPLAY, MINE };
    static const LineKey keys[] = {
        { "frames", MINE }, { "board_opened", MINE }, { "refused", MINE }, { "fb", MINE }, { "drawn", MINE },
        { "frame", MINE }, { "probe_before", MINE }, { "probe_after", MINE }, { "posted", MINE }, { "exit", MINE },
        { "display", DISPLAY },
    };
    emit_line("PS4WINE d4 winemine", keys, sizeof(keys) / sizeof(keys[0]));
}

/* Milestone d's own line. */
static void d_lines(void)
{
    enum { DISPLAY, PLAY };
    static const LineKey keys[] = {
        { "frames", PLAY }, { "inputs", PLAY }, { "refused", PLAY }, { "board_opened", PLAY }, { "fb", PLAY },
        { "drawn", PLAY }, { "pad", PLAY }, { "pad_reads", PLAY }, { "end", PLAY }, { "probe_before", PLAY },
        { "probe_after", PLAY }, { "lat_avg_ms", PLAY }, { "lat_max_ms", PLAY }, { "present_ms", PLAY },
        { "exit", PLAY }, { "display", DISPLAY },
    };
    emit_line("PS4WINE d winemine", keys, sizeof(keys) / sizeof(keys[0]));
}

/* The gated fields and a few measured ones: one netlog datagram carries
 * at most ~512 bytes (e1's first passing line, 523 bytes, lost its build=
 * and stages=). The device name and the memory fields are in the stage
 * records. */
static void e1_lines(void)
{
    enum { LOAD, CAPS, DISPLAY, RELEASE, PRESENT, HELLO };
    static const LineKey keys[] = {
        { "vk_load", LOAD }, { "vk_bound", LOAD }, { "inst", CAPS }, { "api", CAPS }, { "maintenance5", CAPS },
        { "robustness2", CAPS }, { "gpl", CAPS }, { "dyn_rendering", CAPS }, { "xfb", CAPS }, { "display", DISPLAY },
        { "release", RELEASE }, { "present", PRESENT }, { "frames", PRESENT }, { "gpu_hang", PRESENT },
        { "swapchain", PRESENT }, { "fps", PRESENT }, { "fb", PRESENT }, { "reopen", PRESENT }, { "hello", HELLO },
    };
    emit_line("PS4WINE e1 vk", keys, sizeof(keys) / sizeof(keys[0]));
}

/* gc1: the four cursor calls' statuses on the title's VideoOut (the picture
 * is the HDMI recording's), then hello.exe. */
static void gc1_lines(void)
{
    enum { CURSOR, HELLO };
    static const LineKey keys[] = {
        { "cursor", CURSOR }, { "mem", CURSOR }, { "mem_align", CURSOR }, { "mem_rc", CURSOR }, { "enable", CURSOR },
        { "position", CURSOR }, { "image", CURSOR }, { "disable", CURSOR }, { "enable_twice", CURSOR },
        { "position_moved", CURSOR }, { "position_back", CURSOR }, { "position_edge", CURSOR },
        { "enable_again", CURSOR }, { "reopen", CURSOR }, { "reopen_disable", CURSOR }, { "flips", CURSOR },
        { "flip_errors", CURSOR }, { "hello", HELLO },
    };
    emit_line("PS4WINE gc1 cursor", keys, sizeof(keys) / sizeof(keys[0]));
}

/* gc2: libvulkan.prx's pw_videoout_cursor over RADV's frames. */
static void gc2_lines(void)
{
    enum { LOAD, CAPS, DISPLAY, RELEASE, CURSOR, HELLO };
    static const LineKey keys[] = {
        { "present", CURSOR }, { "frames", CURSOR }, { "gpu_hang", CURSOR }, { "swapchain", CURSOR },
        { "reopen", CURSOR }, { "cursor_export", CURSOR }, { "cursor_shows", CURSOR },
        { "cursor_reopen_shows", CURSOR }, { "cursor_hides", CURSOR }, { "cursor_noout", CURSOR },
        { "cursor_errors", CURSOR }, { "cursor_rc", CURSOR }, { "vk_cursor_updates", CURSOR },
        { "vk_cursor_rc", CURSOR }, { "vk_cursor_format", CURSOR }, { "fb_cursor", CURSOR }, { "release", RELEASE },
        { "hello", HELLO },
    };
    emit_line("PS4WINE gc2 vkcursor", keys, sizeof(keys) / sizeof(keys[0]));
}

/* e2: winevulkan and Wine's Vulkan under vk64.exe; e3: win32u's surface,
 * the hand-over, the Win32 swapchain's frames. */
static void e2_lines(void)
{
    enum { W32U, WV, DISPLAY, VK64 };
    static const LineKey e2_keys[] = {
        { "inst", VK64 }, { "api", VK64 }, { "exts_dxvk", VK64 }, { "dev", VK64 }, { "submit", VK64 },
        { "wv_bound", WV }, { "w32u_bound", W32U }, { "wv_driver", WV }, { "wv_first", WV }, { "exts", VK64 },
        { "device", VK64 }, { "exit", VK64 },
    };
    static const LineKey e3_keys[] = {
        { "release", VK64 }, { "surface", VK64 }, { "present", VK64 }, { "frames", VK64 }, { "gpu_hang", VK64 },
        { "swapchain", VK64 }, { "fb", VK64 }, { "app_frames", VK64 }, { "app_swapchain", VK64 }, { "fmt", VK64 },
        { "device_lost", VK64 }, { "gdi_dropped", VK64 }, { "vk_stats", VK64 }, { "display", DISPLAY },
        { "exit", VK64 },
    };
    emit_line("PS4WINE e2 vk64", e2_keys, sizeof(e2_keys) / sizeof(e2_keys[0]));
    emit_line("PS4WINE e3 vkclear", e3_keys, sizeof(e3_keys) / sizeof(e3_keys[0]));
}

/* Milestone e's own line. */
static void e_lines(void)
{
    enum { DISPLAY, D3D11 };
    static const LineKey keys[] = {
        { "frames", D3D11 }, { "gpu_hang", D3D11 }, { "swapchain", D3D11 }, { "fb", D3D11 }, { "px_c", D3D11 },
        { "px_m", D3D11 }, { "hr", D3D11 }, { "at", D3D11 }, { "app_frames", D3D11 }, { "ms_avg", D3D11 },
        { "level", D3D11 }, { "release", D3D11 }, { "device_lost", D3D11 }, { "gdi_dropped", D3D11 },
        { "vk_stats", D3D11 }, { "display", DISPLAY }, { "exit", D3D11 }, { "flex_mib_start", D3D11 },
        { "flex_mib", D3D11 }, { "threads_created", D3D11 }, { "threads_raised", D3D11 },
        { "threads_create_failed", D3D11 }, { "stack_floor_mib", D3D11 }, { "thread_stats", D3D11 },
        { "stack_policy", D3D11 }, { "threads_kept", D3D11 }, { "threads_small", D3D11 },
        { "threads_keep_refused", D3D11 }, { "vk_threads_created", D3D11 }, { "vk_threads_raised", D3D11 },
        { "vk_stack_floor_kib", D3D11 }, { "vk_thread_stats", D3D11 },
    };
    emit_line("PS4WINE e d3d11", keys, sizeof(keys) / sizeof(keys[0]));
}

/* f1: wow32.exe's checks on the translator; f2: its GDI picture through
 * wow64win (gdi64.exe's, built for i686). */
static void f1_lines(void)
{
    enum { WOW, LOW, DISPLAY, WOW32 };
    static const LineKey f1_keys[] = {
        { "cpu", WOW32 }, { "wow_bound", WOW }, { "fp", WOW32 }, { "thread", WOW32 }, { "seh", WOW32 },
        { "faults_unresolved", WOW32 }, { "faults_guest", WOW32 }, { "faults_marked", WOW32 },
        { "faults_host", WOW32 }, { "faults_smc", WOW32 }, { "dbt_errors", WOW32 }, { "wow_threads", WOW32 },
        { "exit", WOW32 }, { "hello", WOW32 }, { "wow_first", WOW }, { "wow_hook", WOW }, { "low_free_mib", LOW },
        { "low_hole", LOW }, { "low_maps", LOW },
    };
    static const LineKey f2_keys[] = {
        { "pixel", WOW32 }, { "window", WOW32 }, { "screen", WOW32 }, { "frames", WOW32 }, { "drawn", WOW32 },
        { "frame", WOW32 }, { "fb", WOW32 }, { "fonts", WOW32 }, { "server", WOW32 }, { "display", DISPLAY },
        { "exit", WOW32 },
    };
    char head[96], pixel[32];

    emit_line("PS4WINE f1 hello32", f1_keys, sizeof(f1_keys) / sizeof(f1_keys[0]));
    value(WOW32, "pixel", pixel, sizeof(pixel));
    /* win32u=ok: the program got as far as drawing and reading back a pixel. */
    snprintf(head, sizeof(head), "PS4WINE f2 gdi32 win32u=%s",
             strcmp(pixel, "-") && strcmp(pixel, "crash") && strcmp(pixel, "hang") ? "ok" : "fail");
    emit_line(head, f2_keys, sizeof(f2_keys) / sizeof(f2_keys[0]));
}

/* Milestone f's own line: Pinball on the translator. */
static void f_lines(void)
{
    enum { WOW, DISPLAY, PINBALL };
    static const LineKey keys[] = {
        { "cpu", PINBALL }, { "frames", PINBALL }, { "fps", PINBALL }, { "faults_unresolved", PINBALL },
        { "native_caps", PINBALL }, { "play_s", PINBALL }, { "inputs", PINBALL }, { "keys", PINBALL },
        { "pad", PINBALL }, { "end", PINBALL }, { "preset", PINBALL }, { "faults_guest", PINBALL },
        { "faults_marked", PINBALL }, { "faults_host", PINBALL }, { "faults_smc", PINBALL },
        { "dbt_errors", PINBALL }, { "refused", PINBALL }, { "present_ms", PINBALL }, { "fb", PINBALL },
        { "frame", PINBALL }, { "wow_bound", WOW }, { "display", DISPLAY }, { "exit", PINBALL },
    };
    emit_line("PS4WINE f pinball", keys, sizeof(keys) / sizeof(keys[0]));
}

/* Phase p's p1: Pinball's attract mode measured over the segment (p_script);
 * the segment's fields come from p_bench, not from the stage file. */
static void p1_lines(void)
{
    enum { WOW, DISPLAY, PINBALL };
    static const LineKey keys[] = {
        { "cpu", PINBALL }, { "faults_unresolved", PINBALL }, { "faults_guest", PINBALL },
        { "faults_marked", PINBALL }, { "faults_host", PINBALL }, { "faults_smc", PINBALL },
        { "dbt_errors", PINBALL }, { "frames", PINBALL }, { "pad", PINBALL }, { "fb", PINBALL },
        { "frame", PINBALL }, { "wow_bound", WOW }, { "display", DISPLAY }, { "exit", PINBALL },
    };
    emit_line_with("PS4WINE p1 bench pinball", keys, sizeof(keys) / sizeof(keys[0]), p_bench[0] ? p_bench : NULL);
}

/* p3 and p4: p1's fields, the display's mode and the test pattern's flips. */
static void p34_line(const char *head)
{
    enum { WOW, DISPLAY, PATTERN, PINBALL };
    static const LineKey keys[] = {
        { "cpu", PINBALL }, { "faults_unresolved", PINBALL }, { "faults_guest", PINBALL },
        { "faults_marked", PINBALL }, { "faults_host", PINBALL }, { "faults_smc", PINBALL },
        { "dbt_errors", PINBALL }, { "frames", PINBALL }, { "pad", PINBALL }, { "fb", PINBALL },
        { "frame", PINBALL }, { "wow_bound", WOW }, { "display", DISPLAY }, { "vo_register", DISPLAY },
        { "vo_size", DISPLAY }, { "vo_pitch", DISPLAY }, { "vo_format", DISPLAY }, { "vo_buffers", DISPLAY },
        { "vo_flip", PATTERN }, { "pattern_presents", PATTERN }, { "exit", PINBALL },
    };
    emit_line_with(head, keys, sizeof(keys) / sizeof(keys[0]), p_bench[0] ? p_bench : NULL);
}

static void p3_lines(void) { p34_line("PS4WINE p3 bench pinball"); }

/* p5: the audio stage's fields, then p1's (with the audio counters). */
static void p5_lines(void)
{
    enum { WOW, DISPLAY, AUDIO, PINBALL };
    static const LineKey keys[] = {
        { "audio", AUDIO }, { "audio_open", AUDIO }, { "audio_user", AUDIO }, { "audio_handle", AUDIO },
        { "audio_init", AUDIO }, { "audio_volume", AUDIO }, { "audio_initial_user", AUDIO },
        { "cpu", PINBALL }, { "faults_unresolved", PINBALL }, { "frames", PINBALL }, { "pad", PINBALL },
        { "fb", PINBALL }, { "frame", PINBALL }, { "wow_bound", WOW }, { "display", DISPLAY },
        { "vo_size", DISPLAY }, { "vo_buffers", DISPLAY }, { "flex_mib_start", PINBALL }, { "flex_mib", PINBALL },
        { "threads_created", PINBALL }, { "threads_raised", PINBALL }, { "threads_create_failed", PINBALL },
        { "stack_floor_mib", PINBALL }, { "thread_stats", PINBALL }, { "stack_policy", PINBALL },
        { "threads_kept", PINBALL }, { "threads_small", PINBALL }, { "threads_keep_refused", PINBALL },
        { "exit", PINBALL },
    };
    emit_line_with("PS4WINE p5 bench pinball", keys, sizeof(keys) / sizeof(keys[0]), p_bench[0] ? p_bench : NULL);
}

/* p5w: wincb.exe's summary, its hold probe and the thread stacks' cost. */
static void p5w_lines(void)
{
    enum { WOW, DISPLAY, WINCB };
    static const LineKey keys[] = {
        { "bits", WINCB }, { "cpu", WINCB }, { "cases", WINCB }, { "cases_ok", WINCB }, { "main_ok", WINCB },
        { "new_ok", WINCB }, { "direct_main", WINCB }, { "direct_new", WINCB }, { "cbt_main", WINCB },
        { "cbt_new", WINCB }, { "cross", WINCB }, { "hold_windows", WINCB }, { "hold2_windows", WINCB },
        { "first_fail", WINCB }, { "faults_unresolved", WINCB }, { "hold_flex_kib", WINCB },
        { "hold_back_kib", WINCB }, { "hold2_flex_kib", WINCB }, { "flex_mib_hold", WINCB },
        { "threads_created_hold", WINCB }, { "flex_mib_start", WINCB }, { "flex_mib", WINCB },
        { "threads_created", WINCB }, { "threads_raised", WINCB }, { "threads_create_failed", WINCB },
        { "stack_floor_mib", WINCB }, { "thread_stats", WINCB }, { "stack_policy", WINCB }, { "threads_kept", WINCB },
        { "threads_small", WINCB }, { "threads_keep_refused", WINCB }, { "wow_bound", WOW }, { "display", DISPLAY },
        { "exit", WINCB },
    };
    emit_line("PS4WINE p5w wincb", keys, sizeof(keys) / sizeof(keys[0]));
}

/* p6: the thread stacks: libkernel's own (stack_probe), then p5w's fields
 * with both holds under the caller-stack policy. */
static void p6_lines(void)
{
    enum { PROBE, WOW, DISPLAY, WINCB };
    static const LineKey keys[] = {
        { "stack_policy", WINCB }, { "hold_flex_kib", WINCB }, { "hold_back_kib", WINCB },
        { "hold2_flex_kib", WINCB }, { "threads_kept", WINCB }, { "threads_small", WINCB },
        { "threads_keep_refused", WINCB }, { "threads_created", WINCB }, { "threads_raised", WINCB },
        { "threads_create_failed", WINCB }, { "stack_floor_mib", WINCB }, { "thread_stats", WINCB },
        { "flex_mib_start", WINCB }, { "flex_mib_hold", WINCB }, { "flex_mib", WINCB },
        { "threads_created_hold", WINCB }, { "sp_lib_rc", PROBE }, { "sp_lib_alive1_kib", PROBE },
        { "sp_lib_kib1", PROBE }, { "sp_lib_alive2_kib", PROBE }, { "sp_lib_kib2", PROBE },
        { "bits", WINCB }, { "cpu", WINCB }, { "cases", WINCB }, { "cases_ok", WINCB }, { "main_ok", WINCB },
        { "new_ok", WINCB }, { "direct_main", WINCB }, { "direct_new", WINCB }, { "cbt_main", WINCB },
        { "cbt_new", WINCB }, { "cross", WINCB }, { "hold_windows", WINCB }, { "hold2_windows", WINCB },
        { "first_fail", WINCB }, { "faults_unresolved", WINCB }, { "wow_bound", WOW }, { "display", DISPLAY },
        { "exit", WINCB },
    };
    emit_line("PS4WINE p6 stacks", keys, sizeof(keys) / sizeof(keys[0]));
}

/* p2: the probe's fields, Wine's native caps (0883's export; "-" while the
 * Orbis ntdll has none), then p1's fields. */
static void p2_lines(void)
{
    enum { WOW, DISPLAY, PROBE, PINBALL };
    static const LineKey keys[] = {
        { "native_caps", PINBALL }, { "cpu", PINBALL }, { "faults_unresolved", PINBALL },
        { "probe_caps", PROBE }, { "stub", PROBE }, { "raw_syscall", PROBE }, { "altstack_lib", PROBE },
        { "altstack53", PROBE }, { "raw_sigaction", PROBE }, { "hits", PROBE }, { "onstack", PROBE },
        { "lookup", PROBE }, { "frame_check", PROBE }, { "mxcsr", PROBE }, { "fsbase", PROBE }, { "stub_bytes", PROBE },
        { "faults_guest", PINBALL }, { "faults_marked", PINBALL }, { "faults_host", PINBALL },
        { "faults_smc", PINBALL }, { "dbt_errors", PINBALL }, { "frames", PINBALL }, { "pad", PINBALL },
        { "fb", PINBALL }, { "frame", PINBALL }, { "wow_bound", WOW }, { "display", DISPLAY }, { "exit", PINBALL },
    };
    emit_line_with("PS4WINE p2 bench pinball", keys, sizeof(keys) / sizeof(keys[0]), p_bench[0] ? p_bench : NULL);
}
static void p4_lines(void) { p34_line("PS4WINE p4 bench pinball"); }

/* ---- milestone g: the lines and the play ------------------------------------------- */

/* g1n: wow32.exe on the native CPU, f1's checks; cpu, native_caps and the
 * unresolved faults come from g_extra. */
static void g1n_lines(void)
{
    enum { PROBE, WOW, LOW, DISPLAY, WOW32 };
    static const LineKey keys[] = {
        { "wow_bound", WOW }, { "fp", WOW32 }, { "thread", WOW32 }, { "seh", WOW32 }, { "hello", WOW32 },
        { "exit", WOW32 }, { "wow_threads", WOW32 }, { "faults_guest", WOW32 }, { "faults_marked", WOW32 },
        { "faults_host", WOW32 }, { "low_free_mib", LOW }, { "display", DISPLAY }, { "cpu_probe", PROBE },
        { "census_threads", PROBE }, { "name_off", PROBE }, { "runtime_off", PROBE }, { "tid_off", PROBE },
        { "probe_runtime_us", PROBE }, { "probe_self_us", PROBE },
    };
    emit_line_with("PS4WINE g1n hello32", keys, sizeof(keys) / sizeof(keys[0]), g_extra);
}

/* g1m: vk32.exe (wine_vk64.c for i686), e2's and e3's fields in one line. */
static void g1m_lines(void)
{
    enum { PROBE, W32U, WV, DISPLAY, VK32 };
    static const LineKey keys[] = {
        { "bits", VK32 }, { "inst", VK32 }, { "api", VK32 }, { "dev", VK32 }, { "submit", VK32 }, { "map", VK32 },
        { "surface", VK32 }, { "present", VK32 }, { "frames", VK32 }, { "gpu_hang", VK32 }, { "swapchain", VK32 },
        { "fb", VK32 }, { "app_frames", VK32 }, { "device_lost", VK32 }, { "release", VK32 }, { "vk_stats", VK32 },
        { "wv_bound", WV }, { "w32u_bound", W32U }, { "display", DISPLAY }, { "exit", VK32 },
        { "cpu_probe", PROBE },
    };
    emit_line_with("PS4WINE g1m vk32", keys, sizeof(keys) / sizeof(keys[0]), g_extra);
}

/* g1: d3d9tri32.exe under DXVK's 32-bit d3d9, e's fields. */
static void g1_lines(void)
{
    enum { PROBE, DISPLAY, D3D9 };
    static const LineKey keys[] = {
        { "bits", D3D9 }, { "frames", D3D9 }, { "gpu_hang", D3D9 }, { "swapchain", D3D9 }, { "fb", D3D9 },
        { "px_c", D3D9 }, { "px_m", D3D9 }, { "hr", D3D9 }, { "at", D3D9 }, { "app_frames", D3D9 },
        { "ms_avg", D3D9 }, { "device_lost", D3D9 }, { "release", D3D9 }, { "vk_stats", D3D9 },
        { "display", DISPLAY }, { "exit", D3D9 }, { "flex_mib_start", D3D9 }, { "flex_mib", D3D9 },
        { "threads_created", D3D9 }, { "threads_kept", D3D9 }, { "threads_keep_refused", D3D9 },
        { "vk_threads_created", D3D9 }, { "vk_stack_floor_kib", D3D9 }, { "cpu_probe", PROBE },
    };
    emit_line_with("PS4WINE g1 d3d9", keys, sizeof(keys) / sizeof(keys[0]), g_extra);
}

/* g's state, written by g_script and read by g_lines: the marks (the first at
 * the menu, the second in the first map), the window's start, the segments. */
static volatile int g_marks, g2_sent, g_line_sent;
static GSnap g_window_a;
static uint64_t g_window_ns;
static long long g_flex_min = -1;
static char g_seg[2][640];
/* The main thread's CPU time per frame in each second of the window (the census). */
static uint32_t g_main_samples[G_WINDOW_S + 16];
static unsigned g_main_n;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static const char *g_main_p95(char *out, size_t size)
{
    uint32_t sorted[G_WINDOW_S + 16];
    unsigned n = g_main_n < G_WINDOW_S + 16 ? g_main_n : G_WINDOW_S + 16;
    if (!n) { snprintf(out, size, "-"); return out; }
    memcpy(sorted, g_main_samples, n * sizeof(sorted[0]));
    qsort(sorted, n, sizeof(sorted[0]), cmp_u32);
    snprintf(out, size, "%u", sorted[(n * 95 + 99) / 100 - 1]);
    return out;
}   /* int, hub: the latest segment's fields, "" before one */
static uint32_t g_inputs, g_kbm_events, g_pad_events;
static const char *g_input_from = "-";
/* The USB keyboard and mouse (native/pw_hid_ps5.c, the PS5's reader: the same
 * system modules and layouts on the PS4, unmeasured here before g). */
static PwHidPs5 g_hid;
static int g_hid_status = -1;

static void g_hid_log(const char *line) { step("g hid %s", line); }

/* What the player used: kbm, pad, both, or none yet. */
static const char *g_input_used(void)
{
    return g_kbm_events && g_pad_events ? "kbm+pad" : g_kbm_events ? "kbm" : g_pad_events ? "pad" : "none";
}

/* The cursor line beside the menu line: VideoOut's hardware cursor over
 * Bloodlines' DXVK frames, from libvulkan.prx's counters. */
static void gc_emit(int menu, const char *exit_text)
{
    char extra[600], cursor[400];

    cursor_fields(cursor, sizeof(cursor));
    snprintf(extra, sizeof(extra), " menu=%d %s faults_unresolved=%llu exit=%s", menu, cursor, faults_unresolved_g(),
             exit_text);
    emit_line_with("PS4WINE gc vtmb", NULL, 0, extra);
}

/* The menu line: the first mark (menu=1), or with g's line when there was none. */
static void g2_emit(int menu, const char *exit_text)
{
    char extra[600];
    struct pw_vulkan_stats vk;

    if (__atomic_exchange_n(&g2_sent, 1, __ATOMIC_ACQ_REL)) return;
    vulkan_stats(&vk);
    snprintf(extra, sizeof(extra), " menu=%d faults_unresolved=%llu faults_unhandled=%u exit=%s presents=%llu "
             "exe_base=%s vtm=%u bink=%u miles=%u ws2_32=%s cpu=%s profile=%s profile_status=%s dxvk_oom=%u kbd=%s "
             "mouse=%s input=%s", menu,
             faults_unresolved_g(), __atomic_load_n(&wine_unhandled, __ATOMIC_RELAXED), exit_text,
             (unsigned long long)vk.presents, exe_base, g_vtm, g_bink, g_miles,
             module_loaded("ws2_32") == 1 ? "loaded" : "no", wow_cpu(), g_profile_id, g_profile_status, dxvk_oom,
             g_hid.keyboard >= 0 ? "ok" : "no", g_hid.mouse >= 0 ? "ok" : "no", g_input_used());
    emit_line_with("PS4WINE g2 vtmb", NULL, 0, extra);
    gc_emit(menu, exit_text);
}

/* Milestone g's own line: the window from the gameplay mark to now (300 s when
 * g_script sends it), session_s counted from the mark. */
static void g_lines(void)
{
    enum { PROBE, WOW, DISPLAY, VTMB };
    static const LineKey keys[] = { { "exit", VTMB }, { "wow_bound", WOW }, { "display", DISPLAY },
                                    { "cpu_probe", PROBE } };
    char extra[1900], window[600] = "", dmem[80], alias[200], threads[400], vk_threads[160], exit_text[32], p95[24],
         live[64];
    GSnap b;
    uint64_t max_ns = 0;
    long long flex = flex_free();
    size_t n;

    value(VTMB, "exit", exit_text, sizeof(exit_text));
    g2_emit(0, exit_text);
    if (g_marks >= 2) {
        g_snap(&b, 0, &max_ns);
        g_window(&g_window_a, &b, max_ns, "", window, sizeof(window));
    } else {
        snprintf(window, sizeof(window), " fps_avg=- crossings_per_frame=-");
    }
    dmem_fields(dmem, sizeof(dmem));
    alias_fields(alias, sizeof(alias));
    thread_fields(threads, sizeof(threads));
    vk_thread_fields(vk_threads, sizeof(vk_threads));
    live_thread_fields(live, sizeof(live));
    if (flex >= 0 && (g_flex_min < 0 || flex < g_flex_min)) g_flex_min = flex;
    {
        BatchRec last = batch_snapshot(0);
        n = (size_t)snprintf(extra, sizeof(extra), " batch=%d gameplay=%d faults_unresolved=%llu faults_unhandled=%u%s",
                             batch_on(&last), g_marks >= 2, faults_unresolved_g(),
                             __atomic_load_n(&wine_unhandled, __ATOMIC_RELAXED), window);
    }
    if (n < sizeof(extra))
        n += (size_t)snprintf(extra + n, sizeof(extra) - n,
                              " cpu=%s input=%s kbd=%s mouse=%s bindings=%s exe_base=%s flex_mib_start=%lld "
                              "flex_mib_min=%lld flex_mib=%lld %s "
                              "dxvk_oom=%u %s %s %s %s batch_lines=%llu main_cpu_us_p95=%s gpu_us_avg=- "
                              "gpu_us_p95=- ib_dw_max=- wsi_us_max=- heaps_mib=-%s%s",
                              wow_cpu(), g_input_used(), g_hid.keyboard >= 0 ? "ok" : "no",
                              g_hid.mouse >= 0 ? "ok" : "no", g_input_from, exe_base,
                              flex_start >= 0 ? flex_start >> 20 : -1,
                              g_flex_min >= 0 ? g_flex_min >> 20 : -1, flex >= 0 ? flex >> 20 : -1, dmem, dxvk_oom,
                              alias, live, threads, vk_threads, (unsigned long long)batch_lines,
                              g_main_p95(p95, sizeof(p95)),
                              g_seg[0][0] ? g_seg[0] : " int_fps_avg=-", g_seg[1][0] ? g_seg[1] : " hub_fps_avg=-");
    if (n < sizeof(extra))
        /* Sound: the port's grains (pw_ms_audio), non-silent ones and refused writes, and the mouse reads. */
        n += (size_t)snprintf(extra + n, sizeof(extra) - n,
                              " audio=%s grains=%llu grains_nonzero=%llu audio_errors=%llu mouse_reads=%llu "
                              "mouse_records=%llu mouse_rc=%d mouse_count=%d",
                              p_audio != 1 ? "off" : audio_handle >= 0 ? "on" : "fail",
                              (unsigned long long)audio_grains, (unsigned long long)audio_nonzero,
                              (unsigned long long)audio_errors, (unsigned long long)g_hid.mouse_reads,
                              (unsigned long long)g_hid.mouse_records, (int)g_hid.mouse_last_rc,
                              (int)g_hid.mouse_count);
    session_base_ns = g_marks >= 2 ? g_window_ns : 0;
    emit_line_with("PS4WINE g vtmb", keys, sizeof(keys) / sizeof(keys[0]), extra);
    session_base_ns = 0;
}

/* Bloodlines' DS4 bindings when no keyboard and mouse are used: the profile's
 * preset (<root>/input/<preset>.input) under its own [input] lines, else
 * these. The d-pad walks (W A S D), the right stick looks (relative mouse
 * moves), R2/L2 are the mouse buttons, cross jumps, circle uses (E), L1
 * crouches, R1 reloads, options is Escape. The touchpad, square and triangle
 * are the title's marks, never bound. */
static const char g_preset[] =
    "[input]\n"
    "mode = keyboard\n"
    "mouse = right_stick\n"
    "mouse_speed = 1600\n"
    "up = w\ndown = s\nleft = a\nright = d\n"
    "cross = space\ncircle = e\nl1 = ctrl\nr1 = r\n"
    "r2 = mouse_left\nl2 = mouse_right\noptions = escape\nl3 = shift\nr3 = f5\n";

static void g_bindings(PwGameInput *input)
{
    static uint8_t text[4096];
    char path[512];
    FILE *file;

    pw_game_input_init(input);
    g_input_from = "builtin";
    if (g_profile && g_profile->input.preset[0]) {
        snprintf(path, sizeof(path), "%s/input/%s.input", cfg.root, g_profile->input.preset);
        if ((file = fopen(path, "rb"))) {
            size_t got = fread(text, 1, sizeof(text), file);
            fclose(file);
            if (pw_game_input_parse(text, got, input) == PW_OK) g_input_from = "file";
            else { step("g %s does not parse: the built-in bindings", path); pw_game_input_init(input); }
        }
    }
    if (strcmp(g_input_from, "file") && pw_game_input_parse((const uint8_t *)g_preset, sizeof(g_preset) - 1, input))
        step("g: the built-in bindings do not parse");
    if (g_profile) pw_game_input_overlay(input, &g_profile->input);
    pw_game_input_default_mode(input);
    /* The marks are the title's. */
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++)
        if (i == 2 || i == 3 || i == 16) input->bindings[i].kind = PW_GAME_BIND_NONE;
}

/* A segment: SEG_WARM_S after its button, a snapshot, SEG_S later another; the
 * player is to stand still (the inputs between are counted). */
typedef struct GSeg { int kind; uint64_t start_ns; int have_a; GSnap a; uint32_t inputs_a; } GSeg;

static void g_seg_end(GSeg *seg)
{
    static const char *const names[] = { "int", "hub" };
    char window[600], extra[800];
    GSnap b;
    long long flex = flex_free();

    g_snap(&b, 0, NULL);
    g_window(&seg->a, &b, G_NO_MAX, "", window, sizeof(window));
    snprintf(extra, sizeof(extra), " seg=%s%s seg_inputs=%u flex_mib=%lld cpu=%s gpu_us_avg=-", names[seg->kind],
             window, g_inputs - seg->inputs_a, flex >= 0 ? flex >> 20 : -1, wow_cpu());
    emit_line_with("PS4WINE gseg vtmb", NULL, 0, extra);
    g_window(&seg->a, &b, G_NO_MAX, seg->kind ? "hub_" : "int_", window, sizeof(window));
    snprintf(g_seg[seg->kind], sizeof(g_seg[0]), "%s %s_seg_inputs=%u", window, names[seg->kind],
             g_inputs - seg->inputs_a);
    seg->kind = -1;
}

/* A mark (the DS4's touchpad, or Pause on the keyboard): the first at the
 * main menu sends g2's line, the second, with the player in control in the
 * first map, starts g's window. */
static void g_mark(uint64_t now, uint64_t started)
{
    int mark = __atomic_add_fetch(&g_marks, 1, __ATOMIC_ACQ_REL);
    step("g mark %d at t=%llus", mark, (unsigned long long)((now - started) / 1000000000ull));
    if (mark == 1) g2_emit(1, "running");
    if (mark == 2) {
        g_snap(&g_window_a, 1, NULL);
        g_window_ns = g_window_a.ns;
    }
}

extern int32_t sceUserServiceInitialize(const void *parameters);
extern int32_t sceUserServiceGetInitialUser(int32_t *user);

static void *g_script(void *arg)
{
    const uint32_t touchpad = pw_game_button_mask(16), square = pw_game_button_mask(2),
                   triangle = pw_game_button_mask(3);
    PwPadOrbis pad;
    PwPadOrbisState state;
    PwGameInput input;
    PwWinePointer pointer = { 0, 0 };
    GSeg seg = { -1, 0, 0, { 0 }, 0 };
    uint32_t held = 0;
    uint64_t started = cfg.now_ns(), last = started, next_s = started, next_hid = started;
    int32_t user = -1;
    int have_pad;
    (void)arg;

    g_bindings(&input);
    have_pad = (d_pad = pw_pad_orbis_open(&pad) == 0);
    if (have_pad) user = pad.user;
    else if (sceUserServiceInitialize(NULL), sceUserServiceGetInitialUser(&user)) user = -1;
    pw_hid_ps5_preload(g_hid_log);
    g_hid_status = pw_hid_ps5_open(&g_hid, pw_hid_ps5_system_ops(g_hid_log), user);
    step("g play: pad=%s user=%d keyboard=%s mouse=%s bindings=%s (touchpad or Pause: mark the menu, then the "
         "first map; square: interior segment; triangle: hub segment)", have_pad ? "ok" : pad.failed, (int)user,
         g_hid.keyboard >= 0 ? "ok" : "no", g_hid.mouse >= 0 ? "ok" : "no", g_input_from);
    for (;;) {
        uint64_t now = cfg.now_ns();

        if (now >= next_s) {
            struct pw_vulkan_stats vk;
            long long flex = flex_free();
            if (flex >= 0 && (g_flex_min < 0 || flex < g_flex_min)) g_flex_min = flex;
            if (g_marks >= 2 && !g_line_sent && census_size) {
                /* One second of the window: the main thread's CPU time per frame. */
                static uint64_t prev_main, prev_presents;
                static int have_prev;
                uint64_t roles[ROLES];
                vulkan_stats(&vk);
                if (!census_roles(roles)) {
                    if (have_prev && vk.presents > prev_presents && roles[ROLE_MAIN] >= prev_main &&
                        g_main_n < sizeof(g_main_samples) / sizeof(g_main_samples[0]))
                        g_main_samples[g_main_n++] =
                            (uint32_t)((roles[ROLE_MAIN] - prev_main) / (vk.presents - prev_presents));
                    prev_main = roles[ROLE_MAIN];
                    prev_presents = vk.presents;
                    have_prev = 1;
                }
            }
            if ((now - started) / 1000000000ull % 10 == 0) {
                vulkan_stats(&vk);
                step("g t=%llus marks=%d presents=%llu flex_mib=%lld batch_lines=%llu unhandled=%u inputs=%u "
                     "kbm=%u pad=%u seg=%d", (unsigned long long)((now - started) / 1000000000ull), g_marks,
                     (unsigned long long)vk.presents, flex >= 0 ? flex >> 20 : -1, (unsigned long long)batch_lines,
                     __atomic_load_n(&wine_unhandled, __ATOMIC_RELAXED), g_inputs, g_kbm_events, g_pad_events,
                     seg.kind);
            }
            next_s = now + 1000000000ull;
        }
        /* A keyboard or mouse plugged in after the start. */
        if ((g_hid.keyboard < 0 || g_hid.mouse < 0) && now >= next_hid) {
            pw_hid_ps5_retry(&g_hid);
            next_hid = now + 5000000000ull;
        }
        if (g_hid.keyboard >= 0 || g_hid.mouse >= 0) {
            PwHidPs5Poll poll;
            pw_hid_ps5_poll(&g_hid, &poll);
            for (size_t i = 0; i < poll.count; i++) {
                const PwHidEvent *e = &poll.events[i];
                if (e->kind == PW_HID_EVENT_KEY && e->code == 0x13) {   /* Pause: the title's mark */
                    if (e->down) g_mark(now, started);
                    continue;
                }
                post(e->kind == PW_HID_EVENT_KEY ? 1u : 3u, e->code, 0, 0, e->down);   /* KEY, MOUSE_BUTTON */
                g_kbm_events++;
                g_inputs++;
            }
            if (poll.dx || poll.dy) {
                post(2, 0, poll.dx, poll.dy, 0);   /* MOUSE_MOVE: relative pixels */
                g_kbm_events++;
                g_inputs++;
            }
            if (poll.wheel) {
                post(4, 0, poll.wheel, 0, 0);      /* MOUSE_WHEEL: detents */
                g_kbm_events++;
                g_inputs++;
            }
        }
        if (have_pad) {
            memset(&state, 0, sizeof(state));
            if (!pw_pad_orbis_read(&pad, &state)) {
                uint32_t pressed = state.buttons & ~held;
                PwWineInput move;

                d_pad_reads++;
                if (pressed & touchpad) g_mark(now, started);
                if ((pressed & (square | triangle)) && seg.kind < 0) {
                    seg.kind = (pressed & triangle) ? 1 : 0;
                    seg.start_ns = now;
                    seg.have_a = 0;
                    step("g segment %s from t=%llus", seg.kind ? "hub" : "int",
                         (unsigned long long)((now - started) / 1000000000ull));
                }
                if (input.mouse != PW_GAME_STICK_NONE &&
                    pw_wine_pointer_step(&pointer, input.mouse == PW_GAME_STICK_LEFT ? state.left_x : state.right_x,
                                         input.mouse == PW_GAME_STICK_LEFT ? state.left_y : state.right_y,
                                         input.mouse_speed ? input.mouse_speed : 1200,
                                         (uint32_t)((now - last) / 1000), &move)) {
                    post(move.type, move.code, move.x, move.y, 0);
                    g_pad_events++;
                    g_inputs++;
                }
                for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++) {
                    const PwGameBinding *bind = &input.bindings[i];
                    uint32_t mask = pw_game_button_mask(i), down = state.buttons & mask;
                    if (!mask || down == (held & mask)) continue;
                    if (bind->kind == PW_GAME_BIND_KEY) post(1, bind->code, 0, 0, down != 0);
                    else if (bind->kind == PW_GAME_BIND_MOUSE) post(3, bind->code, 0, 0, down != 0);
                    else continue;
                    g_pad_events++;
                    g_inputs++;
                }
                held = state.buttons;
            }
        }
        last = now;
        if (seg.kind >= 0 && !seg.have_a && now - seg.start_ns >= (uint64_t)G_SEG_WARM_S * 1000000000ull) {
            g_snap(&seg.a, 0, NULL);
            seg.inputs_a = g_inputs;
            seg.have_a = 1;
        }
        if (seg.kind >= 0 && seg.have_a &&
            now - seg.start_ns >= (uint64_t)(G_SEG_WARM_S + G_SEG_S) * 1000000000ull)
            g_seg_end(&seg);
        if (g_marks >= 2 && !g_line_sent && now - g_window_ns >= (uint64_t)G_WINDOW_S * 1000000000ull) {
            g_line_sent = 1;
            pw_ms_wine_end("running", "", -1);
        }
        pause_ms(8);
    }
    return NULL;
}

void pw_ms_emit_lines(void)
{
    if (__atomic_exchange_n(&lines_emitted, 1, __ATOMIC_ACQ_REL)) return;
    plan->lines();
    if (pw_stage_finish(&run) == 1) cfg.emit("PW_MS cycle complete: progress file kept as progress.txt.last");
}
