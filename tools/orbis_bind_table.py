#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The import slots one Orbis PRX takes from sibling application PRXs, as a
text table the PS4 title reads to check, slot by slot, what the console
loader wrote there (milestone d1).

An importer is linked against the exporter's ELF (tools/build_wine_orbis.sh
link_prx); create-fself keeps the image layout, so a slot's address on the
console is the module's load base plus the relocation offset less the first
PT_LOAD address. Output:

  module <name>
  first_vaddr 0x<hex>
  slot 0x<offset> <JUMP_SLOT|GLOB_DAT|64> <exporter> <symbol>
  ...

Exit status 1 when an import from a sibling is a data object (--no-data).

Usage: orbis_bind_table.py --elf IMPORTER.shared.elf --exporter NAME=EXPORTER.shared.elf
       [--exporter ...] --out IMPORTER.bind.txt [--no-data]
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path


def run(args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def defined_dynamic(elf):
    """Names the ELF's dynamic table defines."""
    names = set()
    for line in run(["llvm-nm", "-D", "--defined-only", str(elf)]).splitlines():
        parts = line.split()
        if len(parts) >= 3:
            names.add(parts[-1].split("@", 1)[0])
    return names


def undefined_dynamic(elf):
    """{name: type} for the undefined dynamic symbols."""
    out = {}
    for line in run(["llvm-readelf", "--dyn-syms", "--wide", str(elf)]).splitlines():
        parts = line.split()
        if len(parts) < 8 or parts[6] != "UND" or not parts[0].rstrip(":").isdigit():
            continue
        out[parts[7].split("@", 1)[0]] = parts[3]
    return out


def first_vaddr(elf):
    for line in run(["llvm-readelf", "-l", "--wide", str(elf)]).splitlines():
        parts = line.split()
        if parts and parts[0] == "LOAD":
            return int(parts[2], 16)
    raise SystemExit(f"{elf}: no PT_LOAD")


RELOC = re.compile(r"^\s*([0-9a-f]+)\s+[0-9a-f]+\s+R_X86_64_(JUMP_SLOT|GLOB_DAT|64)\s+[0-9a-f]+\s+(\S+)")


def slots(elf):
    """(offset, kind, symbol) for every symbol relocation of the image."""
    out = []
    for line in run(["llvm-readelf", "-r", "--wide", str(elf)]).splitlines():
        m = RELOC.match(line)
        if m:
            out.append((int(m.group(1), 16), m.group(2), m.group(3).split("@", 1)[0]))
    return out


def table(elf, exporters):
    """The lines of the table, and the data imports from siblings."""
    undefined = undefined_dynamic(elf)
    provider = {}
    for name, path in exporters:
        for symbol in defined_dynamic(path):
            provider.setdefault(symbol, name)
    lines = [f"module {Path(elf).name.split('.')[0]}", f"first_vaddr 0x{first_vaddr(elf):x}"]
    data = []
    for offset, kind, symbol in sorted(slots(elf)):
        if symbol not in undefined or symbol not in provider:
            continue
        if undefined[symbol] == "OBJECT":
            data.append(symbol)
        lines.append(f"slot 0x{offset:x} {kind} {provider[symbol]} {symbol}")
    return lines, sorted(set(data))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--elf", required=True, type=Path)
    ap.add_argument("--exporter", action="append", default=[], help="NAME=PATH")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--no-data", action="store_true")
    args = ap.parse_args()
    exporters = []
    for item in args.exporter:
        name, sep, path = item.partition("=")
        if not sep:
            ap.error(f"--exporter {item}: NAME=PATH")
        exporters.append((name, Path(path)))
    lines, data = table(args.elf, exporters)
    args.out.write_text("\n".join(lines) + "\n")
    count = sum(1 for line in lines if line.startswith("slot "))
    print(f"{args.out.name}: {count} slot(s) from {', '.join(n for n, _ in exporters)}")
    if data and args.no_data:
        print(f"{args.elf.name}: data import(s) from a sibling PRX: {', '.join(data)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
