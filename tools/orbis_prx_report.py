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
  data_imports     undefined dynamic OBJECT symbols: a data import between
                   application PRXs is not bound by the loader
  unbound          imports that no OpenOrbis stub library exports: the loader
                   leaves these slots at 0 and the first call faults
  raw_syscalls     functions containing a syscall instruction, except Wine's
                   two dispatchers: a raw system call kills a title
  bytes, sha256    of the PRX

Exit status 1 when a module in --must-bind has unbound imports or no PRX, or
any module has raw syscalls (the build script decides what is fatal); a
module outside --must-bind (win32u until its ntdll imports bind) is recorded.

Usage: orbis_prx_report.py --work DIR --toolchain OO_PS4_TOOLCHAIN --out report.json
       [--wine-commit SHA] [--patches DIR] [--must-bind ntdll,wineserver]
"""
import argparse
import hashlib
import json
import os
import re
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
        if re.search(r"\bsyscall\b", line) and current and current not in DISPATCHERS:
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
    for shared in sorted(prx_dir.glob("*.shared.elf")):
        name = shared.name[: -len(".shared.elf")]
        prx = prx_dir / "sce_module" / f"{name}.prx"
        undefined, objects = dynamic_symbols(shared)
        unresolved = unresolved_from_log(prx_dir / f"{name}.link.log")
        unbound = sorted(n for n in undefined if n not in all_exports)
        syscalls = raw_syscalls(shared)
        module = {
            "built": prx.exists(),
            "unresolved": unresolved,
            "needed": needed(shared),
            "imports": len(undefined),
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
