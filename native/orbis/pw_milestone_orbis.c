/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The PS4 port's milestone launches in the title (pw_milestone_orbis.h). */
#include "pw_milestone_orbis.h"
#include "pw_stage_orbis.h"
#include "pw_videoout_orbis.h"
#include "pw_pad_orbis.h"
#include "pw_wine_prx.h"
#include "pw_wine_display.h"

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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern uint32_t sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags,
                                         const void *option, int *result);
extern int sceKernelGetModuleInfo(int32_t handle, void *info);
extern int sceKernelDlsym(int32_t handle, const char *symbol, void **address);
extern int sceKernelGetModuleList(int32_t *handles, size_t size, size_t *count);

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
static sigjmp_buf stage_jump;
static volatile int stage_active = -1;
static volatile int recovering;
static volatile uint64_t stage_deadline;
static volatile int lines_emitted;
static uint64_t start_ns;
static PwVideoOutOrbis video;
static int video_open;

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

/* Each slot of <name>.bind.txt against what sceKernelDlsym says the
 * exporter exports: bound (equal), zero, lazy (still inside the importer:
 * a PLT entry), other; nosym when the exporter's look-up fails. */
static void check_slots(const char *name, const Module *importer, int32_t exporter, Slots *s, const char *watch)
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
            s->total++;
            if (address < importer->low || address + 8 > importer->high) {
                s->other++;
                state = "outside";
            } else {
                memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
                if (sceKernelDlsym(exporter, symbol, &expected) != 0 || !expected) s->nosym++;
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

    snprintf(shot, sizeof(shot), "%s/shot.raw", cfg.root);
    unlink(shot);
    if ((rc = pw_videoout_orbis_open(&video))) {
        snprintf(fields, size, "display=fail vo_failed=%s:0x%x", video.failed, (unsigned)rc);
        return;
    }
    video_open = 1;
    if ((rc = pw_videoout_orbis_flip(&video))) {
        snprintf(fields, size, "display=fail vo_failed=%s:0x%x", video.failed, (unsigned)rc);
        return;
    }
    step("display open handle=%d", video.handle);
    snprintf(fields, size, "display=ok");
}

/* ---- the present sink ------------------------------------------------------------ */

static pthread_mutex_t present_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t frames, drawn;
static uint32_t frame_width, frame_height;
static uint32_t *last_drawn;   /* the last frame with any pixel drawn, as the driver's BGRA (frame_width x frame_height) */
static uint32_t cleared[PW_VIDEOOUT_ORBIS_BUFFERS][2];   /* the frame size each buffer's border was cleared for */
/* Latency: from the first pad event not yet shown to the next present, and each present's own time. */
static volatile uint64_t input_pending_ns;
static uint64_t lat_n, lat_sum_ns, lat_max_ns, present_n, present_sum_ns;
static uint32_t present_scale, present_x0, present_y0;   /* desktop -> 1920x1080 */
static int fb_saved = -1;      /* -1 not yet, 0 saved, else failed */

/* BGRA from the driver, integer-scaled and centred on the 1920x1080
 * buffer as R,G,B,A. The last frame with any pixel drawn is copied aside
 * and saved as the framebuffer readback when the program ends: the driver
 * presents only on changes (the first d2 launch sent 2 frames for a window
 * shown 2 s), so a frame picked by time may never come. */
int pw_ms_present(void *context, const void *bgra, uint32_t width, uint32_t height, uint32_t stride)
{
    const uint32_t W = PW_VIDEOOUT_ORBIS_WIDTH, H = PW_VIDEOOUT_ORBIS_HEIGHT;
    uint32_t scale, x0, y0, *dst;
    uint64_t t0 = cfg.now_ns(), pending = input_pending_ns;
    int any = 0;

    (void)context;
    if (!video_open || !bgra || !width || !height || width > W || height > H) return -1;
    pthread_mutex_lock(&present_lock);
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
    dst = pw_videoout_orbis_back(&video);
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
    pw_videoout_orbis_flip(&video);
    frames++;
    frame_width = width;
    frame_height = height;
    if (any) {
        drawn++;
        /* The source frame, not the 8 MB scan-out buffer: the readback is built from it when saved. */
        if (!last_drawn) last_drawn = malloc((size_t)W * H * 4);
        if (last_drawn)
            for (uint32_t y = 0; y < height; y++)
                memcpy(last_drawn + (size_t)y * width, (const uint8_t *)bgra + (size_t)y * stride, (size_t)width * 4);
    }
    present_n++;
    present_sum_ns += cfg.now_ns() - t0;
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
    if (fb_saved == -1 && last_drawn) fb_saved = save_drawn_as("shot.raw") ? 1 : 0;
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

/* ---- the plans -------------------------------------------------------------------- */

static void d1_lines(void);
static void d2_lines(void);
static void d4_lines(void);
static void d_lines(void);

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
static const PwMsPlan plans[] = {
    { "d1", d1_stages, sizeof(d1_stages) / sizeof(d1_stages[0]), "C:\\hello.exe", 1, d1_lines },
    { "d2", d2_stages, sizeof(d2_stages) / sizeof(d2_stages[0]), "C:\\gdi64.exe", 0, d2_lines },
    { "d4", d4_stages, sizeof(d4_stages) / sizeof(d4_stages[0]), "C:\\windows\\system32\\winemine.exe", 0, d4_lines },
    { "d", d_stages, sizeof(d_stages) / sizeof(d_stages[0]), "C:\\windows\\system32\\winemine.exe", 0, d_lines },
};

const char *pw_ms_select(const char *name)
{
    plan = &plans[3];   /* the newest milestone */
    for (size_t i = 0; name && i < sizeof(plans) / sizeof(plans[0]); i++)
        if (!strcmp(plans[i].name, name)) plan = &plans[i];
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
        if (sigsetjmp(stage_jump, 1) == 0) plan->stages[i].run(fields, sizeof(fields));
        else snprintf(fields, sizeof(fields), "%s", plan->stages[i].fault_fields);
        stage_active = -1;
        stage_deadline = 0;
        pw_stage_end(&run, i, fields);
    }
}

int pw_ms_wine_should_run(void) { return pw_stage_should_run(&run, wine_stage()); }

void pw_ms_wine_begin(void)
{
    set_handlers(NULL);
    pw_stage_begin(&run, wine_stage());
    if (plan->lines == d4_lines || plan->lines == d_lines) {
        pthread_t thread;
        if (!pthread_create(&thread, NULL, plan->lines == d4_lines ? d4_script : d_script, NULL))
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
    } else if (plan == &plans[0]) {
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
static void emit_line(const char *head, const LineKey *keys, size_t count)
{
    char line[1400], summary[512], v[96];
    size_t n = (size_t)snprintf(line, sizeof(line), "%s", head);

    for (size_t i = 0; i < count && n < sizeof(line); i++)
        n += (size_t)snprintf(line + n, sizeof(line) - n, " %s=%s", keys[i].key,
                              value(keys[i].stage, keys[i].key, v, sizeof(v)));
    pw_stage_summary(&run, summary, sizeof(summary));
    if (n < sizeof(line))
        snprintf(line + n, sizeof(line) - n, " build=%s %s session_s=%llu", cfg.build_id, summary,
                 (unsigned long long)((cfg.now_ns() - start_ns) / 1000000000ull));
    cfg.emit(line);
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

void pw_ms_emit_lines(void)
{
    if (__atomic_exchange_n(&lines_emitted, 1, __ATOMIC_ACQ_REL)) return;
    plan->lines();
    if (pw_stage_finish(&run) == 1) cfg.emit("PW_MS cycle complete: progress file kept as progress.txt.last");
}
