/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_loaddll_orbis.h"

#include <stdio.h>
#include <string.h>

const char *pw_loaddll_file_name(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    return name;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

/* The module's file name in LINE as [*start, *start + *length), and where its " at " ends. */
static const char *loaded_name(const char *line, const char **start, size_t *length)
{
    const char *path = strstr(line, "Loaded L\""), *end, *name;
    if (!path) return NULL;
    path += 9;
    if (!(end = strstr(path, "\" at "))) return NULL;
    name = path;
    for (const char *p = path; p < end; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    *start = name;
    *length = (size_t)(end - name);
    return end + 5;
}

static int same_name(const char *a, size_t length, const char *name)
{
    if (!name || strlen(name) != length || !length) return 0;
    for (size_t i = 0; i < length; i++)
        if (lower(a[i]) != lower(name[i])) return 0;
    return 1;
}

int pw_loaddll_is(const char *line, const char *name)
{
    const char *start;
    size_t length;
    return loaded_name(line, &start, &length) && same_name(start, length, name);
}

int pw_loaddll_base(const char *line, const char *name, char *out, size_t size)
{
    const char *start, *base;
    size_t length, digits = 0;

    if (!(base = loaded_name(line, &start, &length)) || !same_name(start, length, name)) return 0;
    while (digits < 16 && ((base[digits] >= '0' && base[digits] <= '9') || (lower(base[digits]) >= 'a' &&
                                                                              lower(base[digits]) <= 'f')))
        digits++;
    if (!digits) return 0;
    snprintf(out, size, "0x%.*s", (int)digits, base);
    return 1;
}
