#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run patch 0145's lookups relative to a root directory
(ObjectAttributes.RootDirectory) through the PS5 working-directory wrappers,
with the root's descriptor coming from a model of the in-process server: one
opened outside ntdll's wrappers, so ntdll has no path recorded for it, as on
the console. nt_to_unix_file_name finds names, other spellings and names
below a subdirectory, reports misses and names to create, and does so for a
descriptor the server keeps (needs_close 0) and for one whose number ntdll
had recorded for another path. A control build without 0145's calls fails the
same lookups with STATUS_INVALID_HANDLE, the error .NET reported on the
console for each subdirectory of a recursive enumeration.

The patched functions are whole only in a patched file.c, so the lookups run
when the pinned Wine checkout is present (PROSPERO_WINE_SOURCE or
.deps/wine/source, as for test_wine_lookup_misses.py). Without it,
track_root_directory, whole in the patch, still runs against the wrappers."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / "wine/patches"
PATCH = PATCHES / "0145-ntdll-ps5-resolve-names-under-a-root-directory.patch"
CWD = ROOT / "wine/ps5"
WINE_COMMIT = "490f6d5dcbb2a5047345b8af88d114bbcaad69a8"
FILE_C = "dlls/ntdll/unix/file.c"
CALL = "track_root_directory( attr->RootDirectory, root_fd );"


def new_side(text, path):
    """The new side of every hunk that changes path, in order."""
    diff = text.split("diff --git a/" + path + " ", 1)[1].split("\ndiff --git ", 1)[0]
    lines = []
    for hunk in diff.split("\n@@")[1:]:
        lines += [x[1:] for x in hunk.splitlines()[1:] if x.startswith((" ", "+")) or x == ""]
    return "\n".join(lines)


def function(source, signature):
    """The definition that starts with signature, through its closing brace."""
    match = re.search(r"^" + re.escape(signature), source, re.M)
    assert match, signature
    end = source.index("\n{", match.start()) + 2
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def wine_source():
    candidates = [Path(os.environ["PROSPERO_WINE_SOURCE"])] if os.environ.get("PROSPERO_WINE_SOURCE") else []
    candidates.append(ROOT / ".deps/wine/source")
    for candidate in candidates:
        if (candidate / FILE_C).is_file():
            return candidate
    return None


def patched_file_c(source):
    """The pinned file.c with every patch of the series that changes it."""
    pristine = subprocess.run(["git", "-C", str(source), "show", f"{WINE_COMMIT}:{FILE_C}"],
                              capture_output=True, text=True)
    with tempfile.TemporaryDirectory() as directory:
        target = Path(directory) / FILE_C
        target.parent.mkdir(parents=True)
        target.write_text(pristine.stdout if pristine.returncode == 0 else (source / FILE_C).read_text())
        for patch in sorted(PATCHES.glob("[0-9][0-9][0-9][0-9]-*.patch")):
            if f"diff --git a/{FILE_C} " in patch.read_text():
                subprocess.run(["git", "apply", f"--include={FILE_C}", str(patch)], cwd=directory, check=True)
        return target.read_text()


changed = new_side(PATCH.read_text(), FILE_C)
start = changed.index("#if defined(__PROSPERO__) || defined(__ORBIS__)\nextern void pw_cwd_track")
DECLARE = changed[start:changed.index("#endif", start) + len("#endif")]
TRACK = DECLARE + "\n\n" + function(changed, "static void track_root_directory(")
SOURCE = wine_source()
if SOURCE:
    whole = patched_file_c(SOURCE)
    assert DECLARE in whole and function(changed, "static void track_root_directory(") in whole
    lookup_c = new_side((PATCHES / "0160-ntdll-ps5-keep-listings-of-searched-directories.patch").read_text(), FILE_C)
    start = lookup_c.index("#if defined(__PROSPERO__) || defined(__ORBIS__)\n/* wine/ps5/pw_wine_cwd.h")
    MATCH = lookup_c[start:lookup_c.index("#endif", lookup_c.index("static int match_dir_entry")) + len("#endif")]
    NT_TO_UNIX = function(whole, "static NTSTATUS nt_to_unix_file_name(")
    BY_ID = function(whole, "static NTSTATUS file_id_to_unix_file_name(")
    # The root is recorded before anything looks a name up under it.
    assert NT_TO_UNIX.index(CALL) < NT_TO_UNIX.index("lookup_unix_name( root_fd")
    assert BY_ID.index(CALL) < BY_ID.index("fstat( root_fd")
    CODE = "\n\n".join([
        TRACK,
        function(whole, "static BOOLEAN get_dir_case_sensitivity_stat("),
        MATCH,
        function(whole, "static NTSTATUS find_file_in_dir("),
        function(whole, "static NTSTATUS lookup_unix_name("),
        NT_TO_UNIX,
    ])
else:
    CODE = TRACK

HARNESS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pw_wine_cwd.h"

#if defined(__GNUC__) && !defined(__clang__)
/* GCC cannot see that the reduced ntdll_umbstowcs below fills what
 * match_dir_entry reads */
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

typedef unsigned short WCHAR;
typedef unsigned char BOOLEAN;
typedef int BOOL;
typedef unsigned int UINT, DWORD, NTSTATUS;
typedef void *HANDLE;
typedef struct { unsigned short Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { HANDLE RootDirectory; UNICODE_STRING *ObjectName; } OBJECT_ATTRIBUTES;
enum server_fd_type { FD_TYPE_INVALID, FD_TYPE_FILE, FD_TYPE_DIR };
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_REPARSE 0x00000104
#define STATUS_INVALID_HANDLE 0xc0000008
#define STATUS_INVALID_PARAMETER 0xc000000d
#define STATUS_NO_SUCH_FILE 0xc000000f
#define STATUS_NO_MEMORY 0xc0000017
#define STATUS_ACCESS_DENIED 0xc0000022
#define STATUS_OBJECT_TYPE_MISMATCH 0xc0000024
#define STATUS_OBJECT_NAME_INVALID 0xc0000033
#define STATUS_OBJECT_NAME_NOT_FOUND 0xc0000034
#define STATUS_OBJECT_NAME_COLLISION 0xc0000035
#define STATUS_OBJECT_PATH_NOT_FOUND 0xc000003a
#define STATUS_BAD_DEVICE_TYPE 0xc00000cb
#define STATUS_UNSUCCESSFUL 0xc0000001
#define FILE_OPEN 1
#define FILE_CREATE 2
#define FILE_OPEN_IF 3
#define FILE_OVERWRITE 4
#define MAX_DIR_ENTRY_LEN 255
#define INVALID_NT_CHARS '*','?','<','>','|','"'
#define TRACE(...) do { } while (0)

static NTSTATUS errno_to_status( int err )
{
    switch (err)
    {
    case ENOENT: return STATUS_OBJECT_NAME_NOT_FOUND;
    case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
    case EACCES: return STATUS_ACCESS_DENIED;
    case EBADF: return STATUS_INVALID_HANDLE;
    default: return STATUS_UNSUCCESSFUL;
    }
}

/* The in-process server: the descriptor it sends for the root directory is
 * opened outside ntdll's wrappers, so ntdll has no path recorded for it. It
 * either sends a new one each time (needs_close) or keeps one, and it knows
 * the name it opened the directory with. */
#define ROOT_HANDLE ((HANDLE)0x44)
int __real_open( const char *path, int flags, ... );
static char server_dir[1024];
static int kept_fd = -1, name_requests;
static unsigned int server_get_unix_fd( HANDLE handle, unsigned int access, int *fd, int *needs_close,
                                        enum server_fd_type *type, unsigned int *options )
{
    (void)access; (void)options;
    assert( handle == ROOT_HANDLE );
    if (kept_fd >= 0) { *fd = kept_fd; *needs_close = 0; }
    else
    {
        if ((*fd = __real_open( server_dir, O_RDONLY | O_DIRECTORY )) < 0) return STATUS_OBJECT_PATH_NOT_FOUND;
        *needs_close = 1;
    }
    if (type) *type = FD_TYPE_DIR;
    return STATUS_SUCCESS;
}
static unsigned int server_get_unix_name( HANDLE handle, char **unix_name )
{
    assert( handle == ROOT_HANDLE );
    name_requests++;
    return (*unix_name = strdup( server_dir )) ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

#ifdef FULL
/* the unix code page, reduced to ASCII */
static int ntdll_wcstoumbs( const WCHAR *src, DWORD srclen, char *dst, DWORD dstlen, BOOL strict )
{
    DWORD i;
    (void)strict;
    if (srclen > dstlen) return -1;
    for (i = 0; i < srclen; i++) { if (src[i] >= 0x80) return -1; dst[i] = (char)src[i]; }
    return (int)srclen;
}
static int ntdll_umbstowcs( const char *src, DWORD srclen, WCHAR *dst, DWORD dstlen )
{
    DWORD i;
    for (i = 0; i < srclen && i < dstlen; i++) dst[i] = (unsigned char)src[i];
    return (int)i;
}
static WCHAR fold( WCHAR c ) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int wcsnicmp_w( const WCHAR *a, const WCHAR *b, int n )
{
    for (; n > 0; n--, a++, b++) if (fold( *a ) != fold( *b )) return fold( *a ) - fold( *b );
    return 0;
}
static WCHAR *wcschr_w( const WCHAR *s, WCHAR c )
{
    for (; *s; s++) if (*s == c) return (WCHAR *)s;
    return NULL;
}
#define wcsnicmp wcsnicmp_w
#define wcschr wcschr_w
static BOOLEAN is_legal_8dot3_name( const WCHAR *name, int len )
{
    int i, dot = -1;
    if (len > 12) return FALSE;
    for (i = 0; i < len; i++)
    {
        if (name[i] == ' ') return FALSE;
        if (name[i] == '.') { if (dot != -1) return FALSE; dot = i; }
    }
    return dot == -1 ? len <= 8 : dot <= 8 && len - dot - 1 <= 3;
}
static int hash_short_file_name( const WCHAR *name, int length, WCHAR *buffer )
{
    int i, n = 0;
    for (i = 0; i < length && n < 4; i++) if (name[i] != '.' && name[i] != ' ') buffer[n++] = fold( name[i] );
    buffer[n++] = '~';
    for (i = 0; i < 3; i++) buffer[n++] = 'A';
    return n;
}
static BOOLEAN get_dir_case_sensitivity_stat( int root_fd, const char *dir );
static BOOLEAN get_dir_case_sensitivity( int root_fd, const char *dir ) { return get_dir_case_sensitivity_stat( root_fd, dir ); }
static NTSTATUS resolve_reparse_point( int fd, int root_fd, OBJECT_ATTRIBUTES *attr, UNICODE_STRING *nt_name,
        unsigned int nt_pos, unsigned int reparse_len, char **unix_name, int unix_len, int pos, UINT disposition,
        BOOL open_reparse, BOOL is_unix, unsigned int reparse_count )
{
    (void)fd; (void)root_fd; (void)attr; (void)nt_name; (void)nt_pos; (void)reparse_len; (void)unix_name;
    (void)unix_len; (void)pos; (void)disposition; (void)open_reparse; (void)is_unix; (void)reparse_count;
    return STATUS_REPARSE;
}
static NTSTATUS nt_to_unix_file_name_no_root( OBJECT_ATTRIBUTES *attr, UNICODE_STRING *nt_name, char **name_ret,
                                              UINT disposition, BOOL open_reparse, BOOL is_unix )
{
    (void)attr; (void)nt_name; (void)name_ret; (void)disposition; (void)open_reparse; (void)is_unix;
    abort();  /* every lookup here has a root directory */
}
#endif

/*CODE*/

static char root[64];

static const char *at( const char *name )
{
    static char paths[2][1024];
    static int next;
    char *path = paths[next++ & 1];
    snprintf( path, 1024, "%s/prefix/drive_c/%s", root, name );
    return path;
}
static void touch( const char *name )
{
    int fd = open( at( name ), O_CREAT | O_WRONLY, 0644 );
    assert( fd >= 0 && !close( fd ) );
}

#ifdef FULL
/* a name under the root directory, as .NET opens each subdirectory of a tree
 * relative to its parent's handle; out gets the unix name on success */
static NTSTATUS lookup( const char *path, UINT disposition, char *out )
{
    WCHAR wide[256];
    UNICODE_STRING name, nt_name;
    OBJECT_ATTRIBUTES attr = { ROOT_HANDLE, &name };
    char *unix_name = NULL;
    NTSTATUS status;
    int i;

    for (i = 0; path[i]; i++) wide[i] = (unsigned char)path[i];
    name.Buffer = wide;
    name.Length = name.MaximumLength = (unsigned short)(i * sizeof(WCHAR));
    status = nt_to_unix_file_name( &attr, &nt_name, &unix_name, disposition, FALSE );
    if (out) strcpy( out, status == STATUS_SUCCESS || status == STATUS_NO_SUCH_FILE ? unix_name : "" );
    if (status == STATUS_SUCCESS || status == STATUS_NO_SUCH_FILE) free( unix_name );
    return status;
}

static void test_lookups(void)
{
    char found[1024];
    struct stat st;

#ifdef CONTROL
    /* without 0145 every lookup under the root fails as on the console */
    assert( lookup( "a", FILE_OPEN, found ) == STATUS_INVALID_HANDLE );
    assert( lookup( "a\\x.txt", FILE_OPEN, found ) == STATUS_INVALID_HANDLE );
    assert( !name_requests );
    (void)st;
#else
    assert( lookup( "a", FILE_OPEN, found ) == STATUS_SUCCESS && !strcmp( found, "./a" ) );
    assert( lookup( "A", FILE_OPEN, found ) == STATUS_SUCCESS && !strcmp( found, "./a" ) );
    assert( lookup( "a\\x.txt", FILE_OPEN, found ) == STATUS_SUCCESS && !strcmp( found, "./a/x.txt" ) );
    assert( lookup( "A\\X.TXT", FILE_OPEN, found ) == STATUS_SUCCESS && !strcmp( found, "./a/x.txt" ) );
    assert( lookup( "Second Dir", FILE_OPEN, found ) == STATUS_SUCCESS && !strcmp( found, "./Second Dir" ) );
    assert( lookup( "missing", FILE_OPEN, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    assert( lookup( "missing\\x.txt", FILE_OPEN, NULL ) == STATUS_OBJECT_PATH_NOT_FOUND );
    assert( lookup( "new.txt", FILE_CREATE, found ) == STATUS_NO_SUCH_FILE && !strcmp( found, "./new.txt" ) );
    assert( lookup( "A\\X.txt", FILE_CREATE, NULL ) == STATUS_OBJECT_NAME_COLLISION );

    /* A descriptor the server keeps, whose number ntdll had recorded for
     * another path (an earlier descriptor closed outside the wrappers): the
     * root's own name replaces it, and the record goes when ntdll closes it. */
    kept_fd = __real_open( server_dir, O_RDONLY | O_DIRECTORY );
    assert( kept_fd >= 0 );
    pw_cwd_track( kept_fd, "/nonexistent-elsewhere" );
    assert( lookup( "a\\x.txt", FILE_OPEN, found ) == STATUS_SUCCESS && !strcmp( found, "./a/x.txt" ) );
    assert( !fstatat( kept_fd, "a/x.txt", &st, 0 ) );
    assert( !close( kept_fd ) );
    kept_fd = -1;
    assert( name_requests == 10 );
#endif
}
#endif

/* track_root_directory alone: the *at() calls resolve against the server's
 * descriptor once it has run */
static void test_track(void)
{
    struct stat st;
    int fd = __real_open( server_dir, O_RDONLY | O_DIRECTORY );

    assert( fd >= 0 );
    errno = 0;
    assert( fstatat( fd, "a/x.txt", &st, 0 ) == -1 && errno == EBADF );
#ifndef CONTROL
    track_root_directory( ROOT_HANDLE, fd );
    assert( !fstatat( fd, "a/x.txt", &st, 0 ) && S_ISREG( st.st_mode ) );
    pw_cwd_track( fd, "/nonexistent-elsewhere" );
    track_root_directory( ROOT_HANDLE, fd );
    assert( !fstatat( fd, "a/x.txt", &st, 0 ) );
#endif
    assert( !close( fd ) );
    errno = 0;
    fd = __real_open( server_dir, O_RDONLY | O_DIRECTORY );
    assert( fstatat( fd, "a/x.txt", &st, 0 ) == -1 && errno == EBADF );  /* close forgot it */
    assert( !close( fd ) );
}

int main(void)
{
    char prefix[256];

    snprintf( root, sizeof(root), "/tmp/pw-rootdir-XXXXXX" );
    assert( mkdtemp( root ) );
    snprintf( prefix, sizeof(prefix), "%s/prefix", root );
    assert( !mkdir( prefix, 0777 ) && !chdir( prefix ) );
    assert( !mkdir( "drive_c", 0777 ) && !mkdir( at( "Mods" ), 0777 ) );
    assert( !mkdir( at( "Mods/a" ), 0777 ) && !mkdir( at( "Mods/Second Dir" ), 0777 ) );
    touch( "Mods/a/x.txt" );
    snprintf( server_dir, sizeof(server_dir), "%s", at( "Mods" ) );

    name_requests = 0;
#ifdef FULL
    test_lookups();
#endif
    test_track();

    assert( !unlink( at( "Mods/a/x.txt" ) ) && !rmdir( at( "Mods/a" ) ) && !rmdir( at( "Mods/Second Dir" ) ) );
    assert( !rmdir( at( "Mods" ) ) && !rmdir( "drive_c" ) );
    assert( !chdir( "/" ) && !rmdir( prefix ) && !rmdir( root ) );
    return 0;
}
'''


def build_and_run(folder, code, defines):
    wraps = sorted(set(re.findall(r"__wrap_([a-z_]+)", (CWD / "pw_wine_cwd_libc.c").read_text())))
    name = "harness" + "".join("-" + d.lower() for d in defines)
    (folder / f"{name}.c").write_text(HARNESS.replace("/*CODE*/", code))
    command = shlex.split(os.environ.get("CC", "cc")) + shlex.split(os.environ.get("CFLAGS", "-O2"))
    command += ["-std=gnu11", "-pthread", "-D__PROSPERO__"] + [f"-D{d}" for d in defines]
    command += ["-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0", "-Wno-unused-function", "-Wno-unused-parameter",
                "-Wno-sign-compare", f"-I{CWD}"]
    command += [f"-Wl,--wrap={wrap}" for wrap in wraps]
    subprocess.run(command + [str(folder / f"{name}.c"), str(CWD / "pw_wine_cwd.c"),
                              str(CWD / "pw_wine_cwd_libc.c"), "-o", str(folder / name)], check=True)
    subprocess.run([str(folder / name)], check=True)


def main():
    full = ["FULL"] if SOURCE else []
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        build_and_run(folder, CODE, full)
        # The control: the same code without 0145's calls fails as the console did.
        control = CODE.replace(CALL, "")
        assert control != CODE or not SOURCE
        build_and_run(folder, control, full + ["CONTROL"])
    if SOURCE:
        print("wine root directory passed: names, other spellings, names below a subdirectory, misses and "
              "creates under a server descriptor, kept and stale descriptors; the control without 0145 "
              "fails with STATUS_INVALID_HANDLE")
    else:
        print("wine root directory passed: track_root_directory against the wrappers; skipped the lookups "
              "(no pinned Wine checkout)")


if __name__ == "__main__":
    main()
