/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Milestone d1's stages in the PS4 title (pw_d1_orbis.h). */
#include "pw_d1_orbis.h"
#include "pw_stage_orbis.h"
#include "pw_videoout_orbis.h"
#include "pw_pad_orbis.h"
#include "pw_wine_prx.h"

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern uint32_t sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags,
                                         const void *option, int *result);
extern int sceKernelGetModuleInfo(int32_t handle, void *info);
extern int sceKernelDlsym(int32_t handle, const char *symbol, void **address);
extern int sceKernelGetModuleList(int32_t *handles, size_t size, size_t *count);

static const char *const stage_names[PW_D1_STAGES] = {
    "bind_early", "bind_lib", "bind_import", "bind_thunk", "bind_data", "w32u_bind", "videoout", "pad", "hello",
};
static const unsigned stage_limit_s[PW_D1_STAGES] = { 10, 10, 10, 10, 10, 20, 15, 15, 0 };
/* What a stage's fields say when it faulted and was recovered. */
static const char *const stage_fault_fields[PW_D1_STAGES] = {
    "early_load=fault order_needed=-", "lib=fault", "import=fault", "thunk=fault", "data=fault",
    "w32u_load=fault w32u_bound=fault", "videoout=fault fb=fault", "pad=fault pad_cross=0", "hello=fault",
};

static PwD1Config cfg;
static PwStageRun run;
static char progress_path[256];
static sigjmp_buf stage_jump;
static volatile int stage_active = -1;
static volatile int recovering;
static volatile uint64_t stage_deadline;
static volatile int line_emitted;
static uint64_t start_ns;
static PwVideoOutOrbis video;

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
    int n = snprintf(line, sizeof(line), "PW_D1 step ");
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

/* Whether a module of this name is loaded (the loader may load a NEEDED
 * application module by itself). */
static int module_loaded(const char *name)
{
    int32_t handles[64];
    size_t count = 0;
    if (sceKernelGetModuleList(handles, sizeof(handles) / sizeof(handles[0]), &count) != 0) return -1;
    for (size_t i = 0; i < count; i++) {
        Module m;
        if (!module_info(handles[i], &m) && !strcmp(m.name, name)) return 1;
    }
    return 0;
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
    int table;           /* 0 when the table could not be read */
} Slots;

static void slot_note(Slots *s, const char *symbol, const char *kind)
{
    if (!s->first[0]) snprintf(s->first, sizeof(s->first), "%s:%s", symbol, kind);
}

/* Each slot of <name>.bind.txt against what sceKernelDlsym says the
 * exporter exports: bound (equal), zero, lazy (still inside the importer:
 * a PLT entry), other; nosym when the exporter's look-up fails. */
static void check_slots(const char *name, const Module *importer, int32_t exporter, Slots *s)
{
    static char text[32768];
    char path[512];
    uint64_t first_vaddr = 0;
    FILE *file;
    size_t got;

    memset(s, 0, sizeof(*s));
    snprintf(path, sizeof(path), "%s/%s.bind.txt", cfg.module_dir, name);
    if (!(file = fopen(path, "rb"))) return;
    got = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[got] = 0;
    s->table = 1;
    for (char *line = text, *next; line && *line; line = next) {
        unsigned long long offset;
        char kind[16], from[64], symbol[128];
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (sscanf(line, "first_vaddr %llx", &offset) == 1) { first_vaddr = offset; continue; }
        if (sscanf(line, "slot %llx %15s %63s %127s", &offset, kind, from, symbol) != 4) continue;
        {
            uint64_t address = importer->low + offset - first_vaddr, value;
            void *expected = NULL;
            s->total++;
            if (address < importer->low || address + 8 > importer->high) { s->other++; slot_note(s, symbol, "outside"); continue; }
            memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
            if (sceKernelDlsym(exporter, symbol, &expected) != 0 || !expected) {
                s->nosym++;
                slot_note(s, symbol, "nosym");
            }
            if (expected && value == (uint64_t)(uintptr_t)expected) s->bound++;
            else if (!value) { s->zero++; slot_note(s, symbol, "zero"); }
            else if (value >= importer->low && value < importer->high) { s->lazy++; slot_note(s, symbol, "lazy"); }
            else if (expected) { s->other++; slot_note(s, symbol, "other"); }
        }
    }
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
    step("%s slots=%s total=%u bound=%u zero=%u lazy=%u other=%u nosym=%u first=%s", what, slot_state(s),
         s->total, s->bound, s->zero, s->lazy, s->other, s->nosym, s->first[0] ? s->first : "-");
}

/* ---- the stages --------------------------------------------------------------- */

typedef void (*StageFn)(char *fields, size_t size);

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
        if (!module_info(handle, &m) && lib > 0) check_slots("pw_bind_early", &m, lib, &s);
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
    check_slots("pw_bind_use", &m, lib, &before);
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
    check_slots("pw_bind_use", &m, lib, &after);
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
    check_slots("pw_bind_data", &m, lib, &s);
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
    check_slots("win32u", &m, ntdll, &s);
    slot_step("w32u_bind", &s);
    sceKernelDlsym(win32u, "__wine_unix_lib_init", &init);
    snprintf(fields, size, "w32u_load=ok w32u_bound=%u/%u w32u_zero=%u w32u_lazy=%u w32u_other=%u w32u_nosym=%u "
             "w32u_first=%s w32u_init=%d", s.bound, s.total, s.zero, s.lazy, s.other, s.nosym,
             s.first[0] ? s.first : "-", init != NULL);
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

static const StageFn stage_fns[PW_D1_HELLO] = {
    stage_bind_early, stage_bind_lib, stage_bind_import, stage_bind_thunk, stage_bind_data,
    stage_w32u_bind, stage_videoout, stage_pad,
};

/* ---- faults and the watchdog ----------------------------------------------- */

static void stage_fault(int sig, siginfo_t *info, void *context)
{
    uint64_t rip = 0;
    int stage = stage_active;
    char detail[200], where[120];

    memcpy(&rip, (const uint8_t *)context + 224, sizeof(rip));
    snprintf(detail, sizeof(detail), "sig=%d addr=0x%llx rip=%s", sig,
             (unsigned long long)(uintptr_t)(info ? info->si_addr : NULL), pw_orbis_module_of(rip, where, sizeof(where)));
    if (stage < 0 || stage >= PW_D1_HELLO || recovering) {
        /* Outside a stage, or a fault while recovering: record it, leave the
         * stage begun (the next launch skips it), say what is known, idle. */
        if (stage >= 0) pw_stage_mark(&run, (unsigned)stage, 'F', detail);
        else cfg.emit(detail);
        pw_d1_emit_line();
        for (;;) sleep(60);
    }
    recovering = 1;
    pw_stage_mark(&run, (unsigned)stage, 'F', detail);
    siglongjmp(stage_jump, 1);
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
            snprintf(detail, sizeof(detail), "limit=%us", stage_limit_s[stage]);
            pw_stage_mark(&run, (unsigned)stage, 'H', detail);
            pw_d1_emit_line();
            return NULL;   /* the stage's thread cannot be stopped; the title idles in it */
        }
    }
}

/* ---- the run -------------------------------------------------------------------- */

void pw_d1_start(const PwD1Config *config)
{
    char dir[256];
    pthread_t thread;

    cfg = *config;
    start_ns = cfg.now_ns();
    snprintf(dir, sizeof(dir), "%s/d1", cfg.root);
    mkdir(dir, 0777);
    snprintf(progress_path, sizeof(progress_path), "%s/progress.txt", dir);
    pw_stage_init(&run, &stage_ops, progress_path, "PW_D1", stage_names, PW_D1_STAGES);
    if (pw_stage_load(&run)) cfg.emit("PW_D1 progress file not writable; results of this launch are not kept");
    {
        char summary[512], line[600];
        pw_stage_summary(&run, summary, sizeof(summary));
        snprintf(line, sizeof(line), "PW_D1 start %s file=%s", summary, progress_path);
        cfg.emit(line);
    }
    set_handlers(stage_fault);
    pthread_create(&thread, NULL, watchdog, NULL);
}

void pw_d1_title_stages(void)
{
    static char fields[PW_STAGE_FIELDS];

    for (unsigned i = 0; i < PW_D1_HELLO; i++) {
        if (!pw_stage_should_run(&run, i)) continue;
        pw_stage_begin(&run, i);
        fields[0] = 0;
        recovering = 0;
        stage_deadline = cfg.now_ns() + (uint64_t)stage_limit_s[i] * 1000000000ull;
        stage_active = (int)i;
        if (sigsetjmp(stage_jump, 1) == 0) stage_fns[i](fields, sizeof(fields));
        else snprintf(fields, sizeof(fields), "%s", stage_fault_fields[i]);
        stage_active = -1;
        stage_deadline = 0;
        pw_stage_end(&run, i, fields);
    }
}

int pw_d1_hello_should_run(void) { return pw_stage_should_run(&run, PW_D1_HELLO); }

void pw_d1_hello_begin(void)
{
    set_handlers(NULL);
    pw_stage_begin(&run, PW_D1_HELLO);
}

void pw_d1_hello_end(const char *hello)
{
    char fields[64];
    snprintf(fields, sizeof(fields), "hello=%s", hello);
    pw_stage_end(&run, PW_D1_HELLO, fields);
    pw_d1_emit_line();
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

void pw_d1_emit_line(void)
{
    static const struct { const char *key; unsigned stage; } keys[] = {
        { "lib", PW_D1_BIND_LIB }, { "import", PW_D1_BIND_IMPORT }, { "import_slots", PW_D1_BIND_IMPORT },
        { "order_needed", PW_D1_BIND_EARLY }, { "data", PW_D1_BIND_DATA }, { "thunk", PW_D1_BIND_THUNK },
        { "w32u_load", PW_D1_W32U_BIND }, { "w32u_bound", PW_D1_W32U_BIND }, { "w32u_zero", PW_D1_W32U_BIND },
        { "videoout", PW_D1_VIDEOOUT }, { "fb", PW_D1_VIDEOOUT }, { "flip_hz", PW_D1_VIDEOOUT },
        { "pad", PW_D1_PAD }, { "pad_cross", PW_D1_PAD }, { "pad_bytes", PW_D1_PAD }, { "hello", PW_D1_HELLO },
    };
    char line[1400], summary[512], v[96];
    const char *import, *thunk, *bind;
    size_t n;

    if (__atomic_exchange_n(&line_emitted, 1, __ATOMIC_ACQ_REL)) return;
    import = value(PW_D1_BIND_IMPORT, "import", v, sizeof(v));
    bind = !strcmp(import, "ok") ? "import" : NULL;
    thunk = value(PW_D1_BIND_THUNK, "thunk", v, sizeof(v));
    if (!bind) bind = !strcmp(thunk, "ok") ? "thunk" : "none";
    n = (size_t)snprintf(line, sizeof(line), "PS4WINE d1 bind=%s", bind);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && n < sizeof(line); i++)
        n += (size_t)snprintf(line + n, sizeof(line) - n, " %s=%s", keys[i].key, value(keys[i].stage, keys[i].key, v, sizeof(v)));
    pw_stage_summary(&run, summary, sizeof(summary));
    if (n < sizeof(line))
        snprintf(line + n, sizeof(line) - n, " build=%s %s session_s=%llu", cfg.build_id, summary,
                 (unsigned long long)((cfg.now_ns() - start_ns) / 1000000000ull));
    cfg.emit(line);
    if (pw_stage_finish(&run) == 1) cfg.emit("PW_D1 cycle complete: progress file kept as progress.txt.last");
}
