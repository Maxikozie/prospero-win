#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tests/wine_gdi64.c under the host Wine: one "GDI64 ..." line with every
field, the offscreen orange fill read back as ff8000, exit 0. The PS4 runs
the same program for milestones d2 and d3.

The host Wine is headless and font-less (no display driver, no FreeType), so
the window, the fonts and the text are not checked here: the window fails
there (the console's user driver is in win32u, patch 0400) and fonts=0.

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and x86_64-w64-mingw32-gcc; without either
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
SOURCE = ROOT / "tests" / "wine_gdi64.c"
LINE = re.compile(r"^GDI64 screen=\d+x\d+ pixel=[0-9a-f]{6} fonts=\d+ tahoma=[01] text_px=\d+ window=\S+ "
                  r"dir=\d+:\d+ open=\d+ add=\d+:\d+ fonts_after=\d+$")


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compiler = shutil.which("x86_64-w64-mingw32-gcc")
    if not os.access(wine, os.X_OK) or not compiler:
        print("wine gdi64 skipped: needs the host Wine and x86_64-w64-mingw32-gcc")
        return 0
    with tempfile.TemporaryDirectory() as directory:
        exe = Path(directory) / "gdi64.exe"
        subprocess.run([compiler, "-O1", "-Wall", "-Wextra", "-Werror", "-o", str(exe), str(SOURCE),
                        "-lgdi32", "-luser32"], check=True)
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="winemenubuilder.exe=d")
        done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=env, timeout=300)
        assert done.returncode == 0, f"{done.stdout}{done.stderr}"
        line = done.stdout.splitlines()[-1]
        assert LINE.match(line), line
        assert " pixel=ff8000 " in line, line
    print(f"wine gdi64 passed: {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
