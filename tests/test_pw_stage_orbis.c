/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* native/orbis/pw_stage_orbis.c over an in-memory file: a fresh cycle, a
 * crash (B without E) resumed and skipped, a recovered fault, a hang, a
 * torn last line, field look-up, the summary and the rename at the end. */
#include "pw_stage_orbis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char file[PW_STAGE_FILE_MAX];
static int exists, renamed, appends;
static char logged[16][512];
static int log_count;
static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static long mem_read(void *context, const char *path, char *buf, size_t size)
{
    size_t n = strlen(file);
    (void)context; (void)path;
    if (!exists) return -1;
    if (n >= size) n = size - 1;
    memcpy(buf, file, n);
    buf[n] = 0;
    return (long)n;
}

static int mem_append(void *context, const char *path, const char *line)
{
    (void)context; (void)path;
    exists = 1;
    appends++;
    strncat(file, line, sizeof(file) - strlen(file) - 2);
    strcat(file, "\n");
    return 0;
}

static int mem_rename(void *context, const char *from, const char *to)
{
    (void)context;
    CHECK(!strcmp(from, "/p/progress.txt") && !strcmp(to, "/p/progress.txt.last"));
    exists = 0;
    file[0] = 0;
    renamed++;
    return 0;
}

static void mem_log(void *context, const char *line)
{
    (void)context;
    if (log_count < 16) snprintf(logged[log_count++], sizeof(logged[0]), "%s", line);
}

static const PwStageOps ops = { mem_read, mem_append, mem_rename, mem_log, NULL };
static const char *const names[] = { "bind_lib", "bind_data", "videoout", "hello" };

static void fresh(PwStageRun *run)
{
    pw_stage_init(run, &ops, "/p/progress.txt", "PW_D1", names, 4);
    CHECK(pw_stage_load(run) == 0);
}

int main(void)
{
    PwStageRun run;
    char summary[512], value[64];

    /* Launch 1: bind_lib ends, bind_data begins and the process dies. */
    fresh(&run);
    CHECK(run.launches == 1);
    CHECK(pw_stage_should_run(&run, 0));
    pw_stage_begin(&run, 0);
    pw_stage_end(&run, 0, "lib=ok calls=3");
    CHECK(log_count == 1 && !strcmp(logged[0], "PW_D1 stage=bind_lib lib=ok calls=3"));
    pw_stage_begin(&run, 1);
    CHECK(strstr(file, "L:1\nB:bind_lib\nR:bind_lib lib=ok calls=3\nE:bind_lib\nB:bind_data\n") == file);
    CHECK(pw_stage_finish(&run) == 0 && !renamed);

    /* Launch 2: bind_lib comes from the file, bind_data is a crash, the rest run. */
    fresh(&run);
    CHECK(run.launches == 2);
    CHECK(!pw_stage_should_run(&run, 0) && run.from_file[0]);
    CHECK(!pw_stage_should_run(&run, 1) && run.state[1] == PW_STAGE_CRASHED);
    CHECK(pw_stage_value(&run, 0, "lib", value, sizeof(value)) && !strcmp(value, "ok"));
    CHECK(pw_stage_value(&run, 0, "calls", value, sizeof(value)) && !strcmp(value, "3"));
    CHECK(!pw_stage_value(&run, 0, "li", value, sizeof(value)));
    CHECK(pw_stage_should_run(&run, 2));
    /* videoout faults and is recovered: F, then R and E. */
    pw_stage_begin(&run, 2);
    pw_stage_mark(&run, 2, 'F', "sig=11 addr=0x0 rip=eboot+0x1234");
    pw_stage_end(&run, 2, "videoout=fault fb=fault");
    CHECK(strstr(file, "F:videoout sig=11 addr=0x0 rip=eboot+0x1234\nR:videoout videoout=fault fb=fault\nE:videoout\n"));
    /* hello hangs. */
    pw_stage_begin(&run, 3);
    pw_stage_mark(&run, 3, 'H', "limit=180s");
    CHECK(run.state[3] == PW_STAGE_HUNG);
    pw_stage_summary(&run, summary, sizeof(summary));
    CHECK(!strcmp(summary, "stages=2/4 stopped=bind_data:crash,hello:hang launches=2 resumed=bind_lib"));
    CHECK(pw_stage_finish(&run) == 1 && renamed == 1 && !exists);

    /* A new cycle after the rename. */
    fresh(&run);
    CHECK(run.launches == 1 && pw_stage_should_run(&run, 0) && pw_stage_should_run(&run, 1));
    pw_stage_summary(&run, summary, sizeof(summary));
    CHECK(!strcmp(summary, "stages=0/4 stopped=- launches=1 resumed=-"));

    /* A torn last line and unknown stage names are ignored; a hang survives a relaunch. */
    strcpy(file, "L:1\nB:bind_lib\nR:bind_lib lib=ok\nE:bind_lib\nB:nosuch\nB:hello\nH:hello limit=180s\nB:videoout\nR:videoout vid");
    exists = 1;
    pw_stage_init(&run, &ops, "/p/progress.txt", "PW_D1", names, 4);
    pw_stage_load(&run);
    CHECK(run.state[0] == PW_STAGE_DONE && run.state[3] == PW_STAGE_HUNG && run.state[2] == PW_STAGE_CRASHED);
    CHECK(run.fields[2][0] == 0);
    CHECK(run.state[1] == PW_STAGE_PENDING && run.launches == 2);
    pw_stage_summary(&run, summary, sizeof(summary));
    CHECK(!strcmp(summary, "stages=1/4 stopped=videoout:crash,hello:hang launches=2 resumed=bind_lib"));

    if (failures) {
        printf("pw stage: %d failure(s)\n", failures);
        return 1;
    }
    printf("pw stage passed: fresh cycle, crash resumed and skipped, recovered fault, hang, torn line, values, summary, rename\n");
    return 0;
}
