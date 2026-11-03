#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tests/wine_dxdiag.c (32-bit, new WoW64) under the host Wine, and under a
copy of it without wineboot.exe, which is how the PS4 runs every process: a
title cannot create a second process, so ntdll's run_wineboot fails
(c00000bb there) and nothing makes the volatile keys wineboot makes at each
start. Without ComputerName\\ActiveComputerName, GetComputerNameW fails and
with it dxdiagn's DxDiag_SystemInfo, so IDxDiagProvider::Initialize returns
E_FAIL and the DirectX SDK's GetDXVersion finds no DirectX 9.0c: Psychonauts
stops at its startup check. Patch 0905 makes ntdll create that key from the
stored ComputerName\\ComputerName when wineboot cannot be started. Both runs
must print computer=ok init=0x00000000 major=9 minor=0 letter=c.

The copy is hard links under build/ (same file system as .deps); only the
copy's wineboot.exe links and the prefix's own wineboot.exe copies are
removed. Needs the host Wine from
tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and i686-w64-mingw32-gcc; without them
the check is skipped with a message."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_dxdiag.c"
LINE = re.compile(r"^DXDIAG computer=(?P<computer>\S+) init=(?P<init>\S+) major=(?P<major>\S+) "
                  r"minor=(?P<minor>\S+) letter=(?P<letter>\S+)$")
WANT = dict(computer="ok", init="0x00000000", major="9", minor="0", letter="c")


def run(wine: Path, exe: Path, prefix: Path, label: str) -> dict[str, str]:
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG="-all", WINEDLLOVERRIDES="winemenubuilder.exe=d")
    subprocess.run([str(wine.parent / "wineserver"), "-k"], env=env, check=False)   # a fresh start: no volatile keys
    done = subprocess.run([str(wine), str(exe)], capture_output=True, text=True, env=env, timeout=300)
    lines = [line.strip() for line in done.stdout.splitlines() if line.startswith("DXDIAG ")]
    assert len(lines) == 1, f"{label}: exit {done.returncode}: {done.stdout}{done.stderr[-2000:]}"
    match = LINE.match(lines[0])
    assert match, f"{label}: {lines[0]}"
    print(f"{label}: {lines[0]}")
    return match.groupdict()


def main() -> int:
    wine = Path(os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine")))
    compiler = shutil.which("i686-w64-mingw32-gcc")
    if not os.access(wine, os.X_OK) or not compiler:
        print("wine dxdiag skipped: needs the host Wine and i686-w64-mingw32-gcc")
        return 0
    install = wine.parents[2]
    (ROOT / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=ROOT / "build", prefix="dxdiag.") as directory:
        work = Path(directory)
        exe = work / "dxdiag32.exe"
        subprocess.run([compiler, "-O2", "-Wall", "-Werror", "-o", str(exe), str(SOURCE), "-lole32", "-loleaut32",
                        "-ldxguid", "-luuid"], check=True)
        copy = work / "install"
        subprocess.run(["cp", "-al", str(install), str(copy)], check=True)
        removed = 0
        for arch in ("x86_64-windows", "i386-windows"):
            boot = copy / "usr/lib/wine" / arch / "wineboot.exe"
            if boot.exists():
                boot.unlink()
                removed += 1
        assert removed, "no wineboot.exe in the host Wine"
        prefix = work / "pfx"
        for label, binary in (("with wineboot", wine), ("without wineboot", copy / wine.relative_to(install))):
            if binary != wine:
                # The prefix's own copies too (the console's prefix has none: prefix.sh removes them).
                for folder in ("system32", "syswow64"):
                    (prefix / "drive_c/windows" / folder / "wineboot.exe").unlink(missing_ok=True)
            got = run(binary, exe, prefix, label)
            wrong = {key: got[key] for key, value in WANT.items() if got[key] != value}
            assert not wrong, f"{label}: {wrong}"
        subprocess.run([str(wine.parent / "wineserver"), "-k"], env=dict(os.environ, WINEPREFIX=str(prefix)),
                       check=False)
    print("wine dxdiag passed: dxdiagn reports DirectX 9.0c with and without wineboot (the console's case)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
