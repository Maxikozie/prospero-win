#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The stub an Orbis PRX is linked against to import a chosen set of symbols
from a sibling application PRX (win32u from ntdll), as assembly.

The set is (a) every import of a first link of the importer that neither the
importer's static libraries nor an OpenOrbis stub library provides (Wine's
own symbols, and the __wrap_* hooks the exporter defines), plus (b) the
--also names (the malloc family, so the importer's heap is the exporter's,
as on the PS5; its dlfcn comes as __wrap_ names, see below). Each must be a function
the exporter defines; a data object or a thread-local one is refused (the
console binds data imports, measured in d1, but Wine's modules need none),
and so is a name an OpenOrbis stub library the importer NEEDs also exports:
create-fself gives an import to the first NEEDED library exporting it, libkernel first
(measured in d1: win32u's dlsym became libkernel's). Import such a name
under a --wrap name the exporter defines (__wrap_dlsym and the rest in
wine/orbis/pw_wine_dl_orbis.c).

Linking against the exporter's whole dynamic table instead would also bind
libc internals (measured: win32u then took musl's __musl_current_locale TLS
variable and __tls_get_addr from ntdll).

The build assembles the output and links it with ld.lld -shared -soname
EXPORTER.so; create-fself only reads the stub's symbol names (NIDs are
hashes of them) and its soname.

Usage: orbis_import_stub.py --first-link IMPORTER.elf --exporter EXPORTER.elf
       --toolchain OO_PS4_TOOLCHAIN [--also NAME,...] --out STUB.s [--list NAMES.txt]
"""
import argparse
import subprocess
import sys
from pathlib import Path


def run(args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def dyn_table(elf):
    """{name: (type, defined)} from the dynamic symbol table."""
    out = {}
    for line in run(["llvm-readelf", "--dyn-syms", "--wide", str(elf)]).splitlines():
        parts = line.split()
        if len(parts) < 8 or not parts[0].rstrip(":").isdigit():
            continue
        name = parts[7].split("@", 1)[0]
        out[name] = (parts[3], parts[6] != "UND")
    return out


def needed(elf):
    out = []
    for line in run(["llvm-readelf", "-d", str(elf)]).splitlines():
        if "(NEEDED)" in line and "[" in line:
            out.append(Path(line.split("[", 1)[1].rstrip("]")).name)
    return out


def sdk_exports(toolchain, only=None):
    """What the OpenOrbis stub libraries export: every one, or those named in ONLY."""
    names = set()
    for stub in sorted(Path(toolchain, "lib").glob("*.so")):
        if only is not None and stub.name not in only:
            continue
        for line in run(["llvm-nm", "-D", "--defined-only", str(stub)]).splitlines():
            parts = line.split()
            if len(parts) >= 3:
                names.add(parts[-1])
    return names


def choose(first, exporter, sdk, also, needed_sdk):
    """(names, errors). sdk: every stub library's exports; needed_sdk: those of
    the stub libraries the importer NEEDs, the ones create-fself searches."""
    errors = []
    wanted = {n for n, (_, defined) in first.items() if not defined and n not in sdk and n}
    wanted |= set(also)
    names = []
    for name in sorted(wanted):
        kind, defined = exporter.get(name, ("", False))
        if name in needed_sdk:
            errors.append(f"{name}: an SDK stub library the importer needs exports it too, "
                          "and create-fself would import it from there")
        elif not defined:
            errors.append(f"{name}: not defined by the exporter")
        elif kind != "FUNC":
            errors.append(f"{name}: a {kind} symbol, not a function")
        else:
            names.append(name)
    return names, errors


def assembly(names):
    lines = ["\t.text"]
    for name in names:
        lines += [f"\t.globl {name}", f"\t.type {name},@function", f"{name}:", "\tret",
                  f"\t.size {name},.-{name}"]
    lines.append('\t.section .note.GNU-stack,"",@progbits')
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--first-link", required=True, type=Path)
    ap.add_argument("--exporter", required=True, type=Path)
    ap.add_argument("--toolchain", required=True, type=Path)
    ap.add_argument("--also", default="")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--list", type=Path)
    args = ap.parse_args()
    also = [n for n in args.also.split(",") if n]
    names, errors = choose(dyn_table(args.first_link), dyn_table(args.exporter), sdk_exports(args.toolchain),
                           also, sdk_exports(args.toolchain, set(needed(args.first_link))))
    for error in errors:
        print(f"orbis_import_stub: {error}", file=sys.stderr)
    if errors:
        return 1
    args.out.write_text(assembly(names))
    if args.list:
        args.list.write_text("\n".join(names) + "\n")
    print(f"{args.out.name}: {len(names)} import(s) from {args.exporter.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
