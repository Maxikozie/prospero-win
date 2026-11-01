/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Stages of a PS4 test launch with a progress file, so that one stage that
 * crashes or hangs does not lose the others' results (milestone d1).
 *
 * The file holds one line per event, each written and synced before the
 * next step:
 *
 *   L:<n>                  launch n of this cycle began
 *   B:<stage>              the stage began
 *   R:<stage> k=v ...      its result fields
 *   E:<stage>              it ended
 *   F:<stage> <detail>     it faulted (recovered: R and E follow)
 *   H:<stage> <detail>     it ran past its time limit
 *
 * At start the runner reads the file: a stage with E is done and keeps its
 * R fields; one with B and no E is "crash" (or "hang" after H) and is not
 * run again; the rest run. A torn last line (no newline) is ignored. Once
 * every stage is done, crashed or hung, pw_stage_finish renames the file to
 * <path>.last, so the next launch starts a new cycle.
 */
#ifndef PW_STAGE_ORBIS_H
#define PW_STAGE_ORBIS_H

#include <stddef.h>

#define PW_STAGE_MAX 16
#define PW_STAGE_FIELDS 1024   /* p5w's and p6's Wine-stage fields run past 512 bytes */
#define PW_STAGE_FILE_MAX 16384

typedef struct PwStageOps {
    /* The whole file into buf (NUL-terminated); bytes read, or -1 if absent. */
    long (*read_file)(void *context, const char *path, char *buf, size_t size);
    /* Append one line (no newline given) and sync; 0 on success. */
    int (*append_line)(void *context, const char *path, const char *line);
    int (*rename_file)(void *context, const char *from, const char *to);
    /* A record for the logs (netlog and klog), already formatted. */
    void (*log)(void *context, const char *line);
    void *context;
} PwStageOps;

enum {
    PW_STAGE_PENDING,   /* not begun in this cycle */
    PW_STAGE_RUNNING,   /* begun in this launch */
    PW_STAGE_DONE,      /* ended (its fields may say fault) */
    PW_STAGE_CRASHED,   /* begun in an earlier launch, never ended */
    PW_STAGE_HUNG,      /* ran past its limit */
};

typedef struct PwStageRun {
    const PwStageOps *ops;
    char path[256];
    const char *prefix;              /* log record prefix, e.g. "PW_D1" */
    unsigned count;
    const char *names[PW_STAGE_MAX];
    int state[PW_STAGE_MAX];
    int from_file[PW_STAGE_MAX];     /* done in an earlier launch */
    char fields[PW_STAGE_MAX][PW_STAGE_FIELDS];
    unsigned launches;               /* this launch's number in the cycle */
} PwStageRun;

void pw_stage_init(PwStageRun *run, const PwStageOps *ops, const char *path, const char *prefix,
                   const char *const *names, unsigned count);
/* Reads the file and appends this launch's L line; 0, or -1 if the line
 * could not be written (the launch then runs without a record). */
int pw_stage_load(PwStageRun *run);
int pw_stage_should_run(const PwStageRun *run, unsigned stage);
void pw_stage_begin(PwStageRun *run, unsigned stage);
/* Records and logs the fields (space-separated k=v) and ends the stage. */
void pw_stage_end(PwStageRun *run, unsigned stage, const char *fields);
/* An F or H line; H also marks the stage hung. Usable from a signal
 * handler as far as the ops' append is (one write and one sync). */
void pw_stage_mark(PwStageRun *run, unsigned stage, char kind, const char *detail);
/* The value of key in the stage's fields, or NULL. */
const char *pw_stage_value(const PwStageRun *run, unsigned stage, const char *key, char *out, size_t size);
/* "stages=<done>/<count> stopped=<-|stage:crash,...> launches=<n> resumed=<-|stage,...>" */
void pw_stage_summary(const PwStageRun *run, char *out, size_t size);
/* Renames the file to <path>.last when no stage is pending or running;
 * 1 if renamed, 0 if not complete, -1 on a failed rename. */
int pw_stage_finish(PwStageRun *run);

#endif
