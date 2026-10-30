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

Several exporters (winevulkan imports from ntdll and win32u) are given in
priority order as --exporter NAME=ELF, one each: a name goes to the first
exporter that defines it, the --also names to the first exporter, and each
exporter gets its own stub, --out with "{exporter}" replaced by its NAME. A
single --exporter ELF (no NAME=) keeps the old form: --out names the stub.

The build assembles each output and links it with ld.lld -shared -soname
EXPORTER.so; create-fself only reads the stub's symbol names (NIDs are
hashes of them) and its soname.

Usage: orbis_import_stub.py --first-link IMPORTER.elf --exporter [NAME=]EXPORTER.elf...
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


def choose(first, exporters, sdk, also, needed_sdk):
    """({exporter name: names}, errors). exporters: [(name, dyn table)] in
    priority order; sdk: every stub library's exports; needed_sdk: those of
    the stub libraries the importer NEEDs, the ones create-fself searches."""
    errors = []
    wanted = {n for n, (_, defined) in first.items() if not defined and n not in sdk and n}
    wanted |= set(also)
    chosen = {name: [] for name, _table in exporters}
    for name in sorted(wanted):
        candidates = exporters[:1] if name in also else exporters
        found = [(exp, table[name][0]) for exp, table in candidates if table.get(name, ("", False))[1]]
        if name in needed_sdk:
            errors.append(f"{name}: an SDK stub library the importer needs exports it too, "
                          "and create-fself would import it from there")
        elif not found:
            errors.append(f"{name}: not defined by the exporter" + ("s" if len(candidates) > 1 else ""))
        elif found[0][1] != "FUNC":
            errors.append(f"{name}: a {found[0][1]} symbol, not a function")
        else:
            chosen[found[0][0]].append(name)
    return chosen, errors


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
    ap.add_argument("--exporter", required=True, action="append",
                    help="[NAME=]EXPORTER.elf, repeated in priority order")
    ap.add_argument("--toolchain", required=True, type=Path)
    ap.add_argument("--also", default="")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--list", type=Path)
    args = ap.parse_args()
    also = [n for n in args.also.split(",") if n]
    exporters = []
    for spec in args.exporter:
        name, _, elf = spec.rpartition("=")
        exporters.append((name or Path(elf).name.split(".", 1)[0], Path(elf)))
    if len(exporters) > 1 and "{exporter}" not in str(args.out):
        print("orbis_import_stub: several exporters need {exporter} in --out", file=sys.stderr)
        return 1
    chosen, errors = choose(dyn_table(args.first_link), [(name, dyn_table(elf)) for name, elf in exporters],
                            sdk_exports(args.toolchain), also,
                            sdk_exports(args.toolchain, set(needed(args.first_link))))
    for error in errors:
        print(f"orbis_import_stub: {error}", file=sys.stderr)
    if errors:
        return 1
    for name, elf in exporters:
        out = Path(str(args.out).replace("{exporter}", name))
        out.write_text(assembly(chosen[name]))
        print(f"{out.name}: {len(chosen[name])} import(s) from {elf.name}")
    if args.list:
        args.list.write_text("".join(f"{n}\n" for name, _elf in exporters for n in chosen[name]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
