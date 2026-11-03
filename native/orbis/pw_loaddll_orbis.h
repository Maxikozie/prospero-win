/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_LOADDLL_ORBIS_H
#define PW_LOADDLL_ORBIS_H
/*
 * Reading Wine's +loaddll lines on Orbis, for the plans' records: a line is
 *
 *   0024:trace:loaddll:build_module Loaded L"C:\\Games\\Game.exe" at 00400000: native
 *
 * (debugstr_w doubles each backslash). The title compares a module by its file
 * name, case-insensitively, as Windows does.
 */
#include <stddef.h>

/* The file name of a Windows path: what follows its last '\' or '/'. */
const char *pw_loaddll_file_name(const char *path);

/* 1 when LINE reports a load of a module whose file name is NAME; then OUT
 * holds its base as "0x" and the hex digits Wine printed (at most 16). 0
 * otherwise, OUT untouched. */
int pw_loaddll_base(const char *line, const char *name, char *out, size_t size);

/* 1 when LINE reports a load of a module whose file name is NAME. */
int pw_loaddll_is(const char *line, const char *name);

#endif
