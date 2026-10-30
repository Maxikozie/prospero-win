#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/orbis_libc_stubs.py: an archive with one ENOSYS stub and two real
functions yields the stub alone, and a config.h that takes the stub for real
fails the check while one that does not passes."""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "orbis_libc_stubs.py")

# 78 is the console's ENOSYS (FreeBSD), the value the SDK's stubs store; the
# host's own ENOSYS differs on Linux.
SOURCE = r"""
#include <errno.h>
int epoll_create(int size) { (void)size; errno = 78; return -1; }
int pipe2(int fds[2], int flags) { fds[0] = flags; fds[1] = flags + 1; return 0; }
int uname_like(void *p) { if (!p) { errno = 78; return -1; } return 0; }
"""


def which(*names):
    for name in names:
        if shutil.which(name):
            return name
    return None


def run_tool(*args):
    return subprocess.run([sys.executable, TOOL] + list(args), capture_output=True, text=True)


def main():
    cc = which("clang-18", "clang", "cc")
    ar = which("llvm-ar-18", "llvm-ar", "ar")
    objdump = which("llvm-objdump-18", "llvm-objdump", "objdump")
    if not (cc and ar and objdump):
        print("test_orbis_libc_stubs: no compiler, ar or objdump; skipped")
        return 0
    with tempfile.TemporaryDirectory() as work:
        src, obj, lib, config = (os.path.join(work, n) for n in ("stubs.c", "stubs.o", "libc.a", "config.h"))
        with open(src, "w", encoding="utf-8") as handle:
            handle.write(SOURCE)
        subprocess.run([cc, "-O2", "-fPIC", "-c", src, "-o", obj], check=True)
        subprocess.run([ar, "rcs", lib, obj], check=True)

        listed = run_tool("--libc", lib, "--objdump", objdump)
        assert listed.returncode == 0 and listed.stdout.split() == ["epoll_create"], listed

        with open(config, "w", encoding="utf-8") as handle:
            handle.write("#define HAVE_PIPE2 1\n#define HAVE_EPOLL_CREATE 1\n/* #undef HAVE_KQUEUE */\n")
        check = run_tool("--libc", lib, "--objdump", objdump, "--config", config)
        assert check.returncode == 1, check
        assert "HAVE_EPOLL_CREATE" in check.stdout and "HAVE_PIPE2" not in check.stdout, check.stdout

        with open(config, "w", encoding="utf-8") as handle:
            handle.write("#define HAVE_PIPE2 1\n/* #undef HAVE_EPOLL_CREATE */\n")
        check = run_tool("--libc", lib, "--objdump", objdump, "--config", config)
        assert check.returncode == 0 and not check.stdout, check
    print("test_orbis_libc_stubs: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
