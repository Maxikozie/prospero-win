#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""List the ENOSYS stubs of the OpenOrbis SDK's libc.a, or check a configured
Wine config.h against them.

The SDK's libc.a is musl with the system-call layer replaced: a function the
kernel has no entry for is a stub that sets errno to ENOSYS (78) and returns
-1, which Wine's configure cannot tell from a real function (its link test
succeeds). A HAVE_<FUNCTION> that names a stub selects Wine code that always
fails (measured: HAVE_EPOLL_CREATE put the in-process server on an event
loop that never ran). Every stub must be declared absent in the configure
arguments of tools/build_wine_orbis.sh; this script finds them.

  orbis_libc_stubs.py [--libc FILE] [--objdump PROGRAM]           list the stubs
  orbis_libc_stubs.py --config CONFIG_H [--libc FILE] ...         exit 1 and name
                                                                   every HAVE_ that
                                                                   is a stub
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ENOSYS = 0x4E
MAX_INSTRUCTIONS = 9  # push, call errno, store, mov -1, pop, ret (+slack)


def find_objdump(explicit):
    for name in ([explicit] if explicit else []) + ["llvm-objdump-18", "llvm-objdump", "objdump"]:
        if name and shutil.which(name):
            return name
    sys.exit("orbis_libc_stubs: no objdump (install llvm-18)")


def stubs_in(archive, objdump):
    """Names of the functions in ARCHIVE whose whole body stores ENOSYS into errno."""
    out = subprocess.run([objdump, "-d", "-r", archive], capture_output=True, text=True, check=True).stdout
    found = set()
    name, count, enosys = None, 0, False

    def flush():
        if name and enosys and count <= MAX_INSTRUCTIONS:
            found.add(name)

    for line in out.splitlines():
        match = re.match(r"^[0-9a-f]+ <([^>]+)>:$", line)
        if match:
            flush()
            name, count, enosys = match.group(1), 0, False
            continue
        if re.match(r"^\s+[0-9a-f]+:", line):
            count += 1
            if re.search(r"mov[lq]\s+\$0x%x, \(%%rax\)" % ENOSYS, line):
                enosys = True
    flush()
    return found


def have_macros(config_h):
    """The HAVE_<NAME> macros defined to 1 in a config.h, as lower-case names."""
    defined = set()
    with open(config_h, encoding="utf-8") as handle:
        for line in handle:
            match = re.match(r"^#define HAVE_([A-Z0-9_]+) 1$", line)
            if match:
                defined.add(match.group(1).lower())
    return defined


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    toolchain = os.environ.get("OO_PS4_TOOLCHAIN") or os.path.expanduser("~/.local/opt/openorbis")
    parser.add_argument("--libc", default=os.path.join(toolchain, "lib", "libc.a"),
                        help="the SDK's libc.a (default: $OO_PS4_TOOLCHAIN/lib/libc.a)")
    parser.add_argument("--objdump", help="disassembler (default: llvm-objdump-18, llvm-objdump, objdump)")
    parser.add_argument("--config", help="a configured Wine config.h to check against the stubs")
    args = parser.parse_args()
    if not os.path.isfile(args.libc):
        sys.exit("orbis_libc_stubs: no libc.a at %s" % args.libc)
    stubs = stubs_in(args.libc, find_objdump(args.objdump))
    if not args.config:
        for name in sorted(stubs):
            print(name)
        return 0
    taken = sorted(stubs & have_macros(args.config))
    for name in taken:
        print("HAVE_%s names an ENOSYS stub of %s" % (name.upper(), args.libc))
    return 1 if taken else 0


if __name__ == "__main__":
    sys.exit(main())
