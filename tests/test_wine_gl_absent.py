#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tests/wine_gl_absent.c, 32-bit (new WoW64) and 64-bit, under the host Wine
and under a copy of it without x86_64-unix/opengl32.so, which is how the
console's runtime is built (--without-opengl): opengl32, wined3d,
d3dcompiler_47 and d3dx9_40 must all load in both. Without the unix library
opengl32 stays loaded with OpenGL unavailable (patch 0480). Psychonauts'
first launch on the PS4 ended with "opengl32.dll failed to initialize"
(STATUS_DLL_INIT_FAILED) because the console still had a 32-bit opengl32.dll
from before 0480 (same size, so the size-only upload had kept it); this
checks that both builds of the module carry it.

The copy is hard links under build/ (same file system as .deps); only the
copy's opengl32.so link is removed. Needs the host Wine from
tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and both mingw compilers; without them
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
SOURCE = ROOT / "tests" / "wine_gl_absent.c"
LINE = re.compile(r"^GLABSENT bits=(?P<bits>\d+) opengl32=(?P<opengl32>\S+) wined3d=(?P<wined3d>\S+) "
                  r"d3dcompiler_47=(?P<d3dcompiler_47>\S+) d3dx9_40=(?P<d3dx9_40>\S+)$")


def run(wine: Path, exe: Path, prefix: Path, label: str) -> dict[str, str]:
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG="-all", WINEDLLOVERRIDES="winemenubuilder.exe=d")
    done = subprocess.run([str(wine), str(exe)], capture_output=True, text=True, env=env, timeout=300)
    lines = [line.strip() for line in done.stdout.splitlines() if line.startswith("GLABSENT ")]
    assert len(lines) == 1, f"{label}: exit {done.returncode}: {done.stdout}{done.stderr[-2000:]}"
    match = LINE.match(lines[0])
    assert match, f"{label}: {lines[0]}"
    print(f"{label}: {lines[0]}")
    return match.groupdict()


def main() -> int:
    wine = Path(os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine")))
    compilers = {32: shutil.which("i686-w64-mingw32-gcc"), 64: shutil.which("x86_64-w64-mingw32-gcc")}
    if not os.access(wine, os.X_OK) or not all(compilers.values()):
        print("wine gl_absent skipped: needs the host Wine and both mingw compilers")
        return 0
    install = wine.parents[2]   # .../install, holding usr/bin/wine
    (ROOT / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=ROOT / "build", prefix="gl_absent.") as directory:
        work = Path(directory)
        copy = work / "install"
        subprocess.run(["cp", "-al", str(install), str(copy)], check=True)
        gl = copy / "usr/lib/wine/x86_64-unix/opengl32.so"
        assert gl.exists(), f"no {gl} in the host Wine"
        gl.unlink()
        wines = {"with opengl32.so": wine, "without opengl32.so": copy / wine.relative_to(install)}
        prefix = work / "pfx"
        for bits, compiler in compilers.items():
            exe = work / f"gl_absent{bits}.exe"
            subprocess.run([compiler, "-O2", "-Wall", "-Werror", "-o", str(exe), str(SOURCE)], check=True)
            for label, binary in wines.items():
                got = run(binary, exe, prefix, f"{bits}-bit {label}")
                assert got["bits"] == str(bits), f"{bits}-bit {label}: bits={got['bits']}"
                wrong = {key: value for key, value in got.items() if key != "bits" and value != "ok"}
                assert not wrong, f"{bits}-bit {label}: {wrong}"
            subprocess.run([str(wines["with opengl32.so"].parent / "wineserver"), "-k"],
                           env=dict(os.environ, WINEPREFIX=str(prefix)), check=False)
    print("wine gl_absent passed: opengl32, wined3d, d3dcompiler_47 and d3dx9_40 load with and without "
          "opengl32's unix library, 32-bit (WoW64) and 64-bit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
