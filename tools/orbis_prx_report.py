#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""report.json for the Orbis (PS4) Wine build: what each PRX imports and what
the console will not provide.

For every module under WORK/prx with a NAME.shared.elf:
  built            NAME.prx exists under prx/sce_module
  unresolved       symbols ld.lld warned about (--warn-unresolved-symbols),
                   from NAME.link.log
  needed           DT_NEEDED entries (the SDK's stub libraries)
  imports          undefined dynamic symbols (what the loader must bind)
  sce_imports      {library: count} of the import entries create-fself wrote
                   into NAME.oelf; a sibling's count must equal
                   sibling_imports' (create-fself gives a name to the first
                   NEEDED library exporting it, libkernel first: measured in
                   d1, win32u's dlsym went to libkernel)
  data_imports     undefined dynamic OBJECT symbols: a data import between
                   application PRXs is not bound by the loader
  sibling_imports  {module: count} of imports another PRX of the build
                   exports, by its NEEDED soname (win32u from ntdll.so)
  unbound          imports that neither an OpenOrbis stub library nor a
                   sibling PRX named in NEEDED exports: the loader leaves
                   these slots at 0 and the first call faults
  raw_syscalls     functions containing a syscall instruction, except Wine's
                   two dispatchers: a raw system call kills a title
  bytes, sha256    of the PRX

Exit status 1 when a module in --must-bind has unbound imports or no PRX, a
data import from a sibling PRX, or any module has raw syscalls (the build
script decides what is fatal).

Usage: orbis_prx_report.py --work DIR --toolchain OO_PS4_TOOLCHAIN --out report.json
       [--wine-commit SHA] [--patches DIR] [--must-bind ntdll,wineserver]
"""
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

DISPATCHERS = {"__wine_syscall_dispatcher", "__wine_unix_call_dispatcher"}


def run(args):
    return subprocess.run(args, capture_output=True, text=True, check=False).stdout


def stub_exports(toolchain):
    """Every symbol the SDK's stub libraries export, by library."""
    exports = {}
    for stub in sorted(Path(toolchain, "lib").glob("*.so")):
        names = set()
        for line in run(["llvm-nm", "-D", "--defined-only", str(stub)]).splitlines():
            parts = line.split()
            if len(parts) >= 3:
                names.add(parts[-1])
        exports[stub.name] = names
    return exports


def dynamic_symbols(elf):
    """(undefined names, undefined OBJECT names) from the dynamic table."""
    undefined, objects = set(), set()
    for line in run(["llvm-readelf", "--dyn-syms", str(elf)]).splitlines():
        parts = line.split()
        if len(parts) < 8 or parts[6] != "UND" or not parts[0].rstrip(":").isdigit():
            continue
        name = parts[7].split("@", 1)[0]
        undefined.add(name)
        if parts[3] == "OBJECT":
            objects.add(name)
    return undefined, objects


def defined_names(elf):
    names = set()
    for line in run(["llvm-nm", "-D", "--defined-only", str(elf)]).splitlines():
        parts = line.split()
        if len(parts) >= 3:
            names.add(parts[-1].split("@", 1)[0])
    return names


SCE_DYNLIBDATA = 0x61000000
DT_SCE_IMPORT_LIB = 0x61000015
DT_SCE_STRTAB = 0x61000035
DT_SCE_STRSZ = 0x61000037
# The letters of an SCE symbol name's "#<library>#<module>" suffix.
SCE_ID = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"


def sce_import_counts(oelf):
    """{library name: import entries} from an OELF's SCE dynamic tables."""
    data = Path(oelf).read_bytes()
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    dynlib = dynamic = None
    for i in range(phnum):
        p_type, _, offset, _, _, filesz, _, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + i * 56)
        if p_type == SCE_DYNLIBDATA:
            dynlib = offset
        elif p_type == 2:
            dynamic = (offset, filesz)
    if dynlib is None or dynamic is None:
        return {}
    tags = {}
    for at in range(dynamic[0], dynamic[0] + dynamic[1], 16):
        tag, value = struct.unpack_from("<QQ", data, at)
        tags.setdefault(tag, []).append(value)
    strtab = data[dynlib + tags[DT_SCE_STRTAB][0]: dynlib + tags[DT_SCE_STRTAB][0] + tags[DT_SCE_STRSZ][0]]

    def string(at):
        return strtab[at: strtab.index(b"\0", at)].decode()

    letters = {}
    for value in tags.get(DT_SCE_IMPORT_LIB, []):
        library_id = (value >> 48) & 0xffff
        if library_id < len(SCE_ID):
            letters[SCE_ID[library_id]] = string(value & 0xffffffff)
    counts = {}
    for entry in strtab.split(b"\0"):
        parts = entry.decode(errors="replace").split("#")
        if len(parts) == 3 and parts[1] in letters:
            counts[letters[parts[1]]] = counts.get(letters[parts[1]], 0) + 1
    return counts


def needed(elf):
    out = []
    for line in run(["llvm-readelf", "-d", str(elf)]).splitlines():
        m = re.search(r"\(NEEDED\)\s+Shared library: \[(.*)\]", line)
        if m:
            out.append(m.group(1))
    return out


def unresolved_from_log(log):
    names = set()
    if not log.exists():
        return []
    for line in log.read_text(errors="replace").splitlines():
        m = re.search(r"undefined symbol: (\S+)", line)
        if m:
            names.add(m.group(1))
    return sorted(names)


def raw_syscalls(elf):
    """Functions whose code contains a syscall instruction."""
    found, current = set(), None
    for line in run(["llvm-objdump", "-d", "--no-show-raw-insn", str(elf)]).splitlines():
        m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
        if m:
            current = m.group(1)
            continue
        # The mnemonic, not a call to libc's syscall() (an ENOSYS stub on the
        # console): "callq 0x... <syscall>" is not a raw system call.
        if re.match(r"^\s*[0-9a-f]+:\s+syscall\b", line) and current and current not in DISPATCHERS:
            found.add(current)
    return sorted(found)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--work", required=True, type=Path)
    ap.add_argument("--toolchain", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--wine-commit", default="")
    ap.add_argument("--patches", type=Path)
    ap.add_argument("--must-bind", default="ntdll,wineserver")
    args = ap.parse_args()

    exports = stub_exports(args.toolchain)
    all_exports = set().union(*exports.values()) if exports else set()
    prx_dir = args.work / "prx"
    report = {
        "schema": 1,
        "platform": "orbis",
        "wine_commit": args.wine_commit,
        "patches": sorted(p.name for p in args.patches.glob("*.patch")) if args.patches else [],
        "stub_libraries": sorted(exports),
        "modules": {},
        "errors": [],
        "warnings": [],
    }
    must_bind = {n for n in args.must_bind.split(",") if n}
    status = 0
    shared_elves = sorted(prx_dir.glob("*.shared.elf"))
    sibling_exports = {elf.name[: -len(".shared.elf")]: defined_names(elf) for elf in shared_elves}
    for shared in shared_elves:
        name = shared.name[: -len(".shared.elf")]
        prx = prx_dir / "sce_module" / f"{name}.prx"
        undefined, objects = dynamic_symbols(shared)
        unresolved = unresolved_from_log(prx_dir / f"{name}.link.log")
        deps = needed(shared)
        siblings = [d[: -len(".so")] for d in deps if d.endswith(".so") and d[: -len(".so")] in sibling_exports]
        from_sibling = {}
        for symbol in undefined:
            for sibling in siblings:
                if symbol in sibling_exports[sibling]:
                    from_sibling[symbol] = sibling
                    break
        unbound = sorted(n for n in undefined if n not in all_exports and n not in from_sibling)
        syscalls = raw_syscalls(shared)
        sibling_data = sorted(n for n in objects if n in from_sibling)
        module = {
            "built": prx.exists(),
            "unresolved": unresolved,
            "needed": deps,
            "imports": len(undefined),
            "sibling_imports": {s: sum(1 for v in from_sibling.values() if v == s) for s in siblings},
            "sce_imports": sce_import_counts(prx_dir / f"{name}.oelf") if (prx_dir / f"{name}.oelf").exists() else {},
            "data_imports": sorted(objects),
            "unbound": unbound,
            "raw_syscalls": syscalls,
        }
        if prx.exists():
            data = prx.read_bytes()
            module["bytes"] = len(data)
            module["sha256"] = hashlib.sha256(data).hexdigest()
        report["modules"][name] = module
        if not prx.exists():
            report.setdefault("warnings", []).append(f"{name}: no PRX")
            if name in must_bind:
                report["errors"].append(f"{name}: no PRX")
                status = 1
        for sibling, count in module["sibling_imports"].items():
            got = module["sce_imports"].get(sibling)
            if module["sce_imports"] and got != count:
                report["errors"].append(f"{name}: {count} import(s) from {sibling} in the ELF, {got or 0} in the OELF's "
                                        f"{sibling} library (create-fself gave the rest to an SDK library)")
                status = 1
        if sibling_data:
            report["errors"].append(f"{name}: data import(s) from a sibling PRX: {', '.join(sibling_data)}")
            status = 1
        if syscalls:
            report["errors"].append(f"{name}: raw syscall in {', '.join(syscalls)}")
            status = 1
        if name in must_bind and unbound:
            report["errors"].append(f"{name}: {len(unbound)} import(s) no stub library provides: "
                                    + ", ".join(unbound[:20]))
            status = 1
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    for error in report["errors"]:
        print(f"report: {error}", file=sys.stderr)
    return status


if __name__ == "__main__":
    sys.exit(main())
