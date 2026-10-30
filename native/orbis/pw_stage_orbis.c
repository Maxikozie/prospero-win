/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Stages of a PS4 test launch with a progress file (pw_stage_orbis.h). */
#include "pw_stage_orbis.h"

#include <stdio.h>
#include <string.h>

void pw_stage_init(PwStageRun *run, const PwStageOps *ops, const char *path, const char *prefix,
                   const char *const *names, unsigned count)
{
    memset(run, 0, sizeof(*run));
    run->ops = ops;
    snprintf(run->path, sizeof(run->path), "%s", path);
    run->prefix = prefix;
    run->count = count < PW_STAGE_MAX ? count : PW_STAGE_MAX;
    for (unsigned i = 0; i < run->count; i++) run->names[i] = names[i];
    run->launches = 1;
}

static int find_stage(const PwStageRun *run, const char *name, size_t length)
{
    for (unsigned i = 0; i < run->count; i++)
        if (strlen(run->names[i]) == length && !memcmp(run->names[i], name, length)) return (int)i;
    return -1;
}

static void append(PwStageRun *run, const char *line)
{
    run->ops->append_line(run->ops->context, run->path, line);
}

int pw_stage_load(PwStageRun *run)
{
    static char text[PW_STAGE_FILE_MAX];
    int begun[PW_STAGE_MAX] = { 0 }, ended[PW_STAGE_MAX] = { 0 }, hung[PW_STAGE_MAX] = { 0 };
    char line[32];
    long got = run->ops->read_file(run->ops->context, run->path, text, sizeof(text));

    for (char *p = got > 0 ? text : NULL, *next; p && *p; p = next) {
        char *newline = strchr(p, '\n'), *name, *end;
        int stage;
        if (!newline) break;   /* a torn last line */
        *newline = 0;
        next = newline + 1;
        if (p[0] == 'L' && p[1] == ':') { run->launches++; continue; }
        if (!strchr("BREFH", p[0]) || p[1] != ':') continue;
        name = p + 2;
        end = strchr(name, ' ');
        if ((stage = find_stage(run, name, end ? (size_t)(end - name) : strlen(name))) < 0) continue;
        switch (p[0]) {
        case 'B': begun[stage] = 1; ended[stage] = 0; hung[stage] = 0; run->fields[stage][0] = 0; break;
        case 'R': snprintf(run->fields[stage], sizeof(run->fields[stage]), "%s", end ? end + 1 : ""); break;
        case 'E': ended[stage] = 1; break;
        case 'H': hung[stage] = 1; break;
        default: break;
        }
    }
    for (unsigned i = 0; i < run->count; i++) {
        if (ended[i]) { run->state[i] = PW_STAGE_DONE; run->from_file[i] = 1; }
        else if (begun[i]) run->state[i] = hung[i] ? PW_STAGE_HUNG : PW_STAGE_CRASHED;
        else run->state[i] = PW_STAGE_PENDING;
    }
    snprintf(line, sizeof(line), "L:%u", run->launches);
    return run->ops->append_line(run->ops->context, run->path, line) ? -1 : 0;
}

int pw_stage_should_run(const PwStageRun *run, unsigned stage)
{
    return stage < run->count && run->state[stage] == PW_STAGE_PENDING;
}

void pw_stage_begin(PwStageRun *run, unsigned stage)
{
    char line[64];
    if (stage >= run->count) return;
    run->state[stage] = PW_STAGE_RUNNING;
    run->fields[stage][0] = 0;
    snprintf(line, sizeof(line), "B:%s", run->names[stage]);
    append(run, line);
}

void pw_stage_end(PwStageRun *run, unsigned stage, const char *fields)
{
    char line[PW_STAGE_FIELDS + 64];
    if (stage >= run->count || run->state[stage] != PW_STAGE_RUNNING) return;
    snprintf(run->fields[stage], sizeof(run->fields[stage]), "%s", fields ? fields : "");
    snprintf(line, sizeof(line), "R:%s %s", run->names[stage], run->fields[stage]);
    append(run, line);
    snprintf(line, sizeof(line), "E:%s", run->names[stage]);
    append(run, line);
    run->state[stage] = PW_STAGE_DONE;
    snprintf(line, sizeof(line), "%s stage=%s %s", run->prefix, run->names[stage], run->fields[stage]);
    run->ops->log(run->ops->context, line);
}

void pw_stage_mark(PwStageRun *run, unsigned stage, char kind, const char *detail)
{
    char line[256];
    if (stage >= run->count || (kind != 'F' && kind != 'H')) return;
    snprintf(line, sizeof(line), "%c:%s %s", kind, run->names[stage], detail ? detail : "");
    append(run, line);
    if (kind == 'H') run->state[stage] = PW_STAGE_HUNG;
    snprintf(line, sizeof(line), "%s stage=%s %s %s", run->prefix, run->names[stage],
             kind == 'F' ? "fault" : "hang", detail ? detail : "");
    run->ops->log(run->ops->context, line);
}

const char *pw_stage_value(const PwStageRun *run, unsigned stage, const char *key, char *out, size_t size)
{
    size_t length = strlen(key);
    if (stage >= run->count || !size) return NULL;
    for (const char *p = run->fields[stage]; *p; ) {
        const char *end = strchr(p, ' ');
        size_t word = end ? (size_t)(end - p) : strlen(p);
        if (word > length && p[length] == '=' && !memcmp(p, key, length)) {
            size_t n = word - length - 1 < size - 1 ? word - length - 1 : size - 1;
            memcpy(out, p + length + 1, n);
            out[n] = 0;
            return out;
        }
        p += word;
        while (*p == ' ') p++;
    }
    return NULL;
}

static void add_word(char *out, size_t size, size_t *used, const char *word, int *first)
{
    int n = snprintf(out + *used, size - *used, "%s%s", *first ? "" : ",", word);
    if (n > 0 && (size_t)n < size - *used) *used += (size_t)n;
    *first = 0;
}

void pw_stage_summary(const PwStageRun *run, char *out, size_t size)
{
    char stopped[256] = "-", resumed[256] = "-";
    size_t s_used = 0, r_used = 0;
    unsigned done = 0;
    int s_first = 1, r_first = 1;

    for (unsigned i = 0; i < run->count; i++) {
        char word[64];
        if (run->state[i] == PW_STAGE_DONE) done++;
        if (run->state[i] == PW_STAGE_CRASHED || run->state[i] == PW_STAGE_HUNG) {
            snprintf(word, sizeof(word), "%s:%s", run->names[i], run->state[i] == PW_STAGE_HUNG ? "hang" : "crash");
            add_word(stopped, sizeof(stopped), &s_used, word, &s_first);
        }
        if (run->from_file[i]) add_word(resumed, sizeof(resumed), &r_used, run->names[i], &r_first);
    }
    snprintf(out, size, "stages=%u/%u stopped=%s launches=%u resumed=%s", done, run->count, stopped,
             run->launches, resumed);
}

int pw_stage_finish(PwStageRun *run)
{
    char last[sizeof(run->path) + 8];
    for (unsigned i = 0; i < run->count; i++)
        if (run->state[i] == PW_STAGE_PENDING || run->state[i] == PW_STAGE_RUNNING) return 0;
    snprintf(last, sizeof(last), "%s.last", run->path);
    return run->ops->rename_file(run->ops->context, run->path, last) ? -1 : 1;
}
