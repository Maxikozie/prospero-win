#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tests/wine_mapvk.c (32-bit, new WoW64) on the host Wines: MapVirtualKeyExA
must give 0 for every code 0x100-0x2ff under MAPVK_VSC_TO_VK and
MAPVK_VSC_TO_VK_EX (patch 0904), while plain and E0 scan codes still
translate, and the key names of the extended Right Arrow and Right Ctrl must
not change.

  drv     the console's code: .deps/wine-host-drv (the PS4/PS5 user driver,
          so win32u's generic MapVirtualKeyEx) through
          tools/test_wine_ps5_driver.sh, in a copy of its prefix. Always run;
          skipped with a message when the tree, its prefix or
          i686-w64-mingw32-gcc is missing.
  winex11 the PC's: .deps/wine-host-desktop (winex11) in its own prefix
          under build/host/mapvk-prefix-desktop, only with PW_TEST_WINEX11=1
          and DISPLAY set. It opens no window; only the keymap is read.

--record writes each tree's key names to build/host/mapvk-before-<tree>.txt
and reports the line without asserting it (the run before the patch). Without
it the line must say verdict=pass, and where such a file exists the names
must equal it."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_mapvk.c"
DRIVER = ROOT / ".deps/wine-host-drv"
DESKTOP = ROOT / ".deps/wine-host-desktop/install/usr/bin/wine"
WORK = ROOT / "build/host"
LINE = re.compile(r"^MAPVK verdict=(pass|fail) .* name_right=(\S+) name_rctrl=(\S+)$")


def mapvk_line(log: str, tree: str) -> re.Match:
    lines = [line.strip() for line in log.splitlines() if line.startswith("MAPVK ")]
    assert len(lines) == 1, f"{tree}: no single MAPVK line:\n{log[-3000:]}"
    match = LINE.match(lines[0])
    assert match, f"{tree}: {lines[0]}"
    return match


def check(tree: str, match: re.Match, record: bool) -> None:
    names = f"name_right={match.group(2)} name_rctrl={match.group(3)}\n"
    before = WORK / f"mapvk-before-{tree}.txt"
    if record:
        WORK.mkdir(parents=True, exist_ok=True)
        before.write_text(names)
        print(f"wine mapvk {tree} recorded: {match.group(0)}")
        return
    assert match.group(1) == "pass", f"{tree}: {match.group(0)}"
    if before.is_file():
        assert before.read_text() == names, f"{tree}: key names changed: was {before.read_text().strip()}, now {names.strip()}"
    print(f"wine mapvk {tree} passed: {match.group(0)}" + ("" if before.is_file() else " (no recorded key names)"))


def main() -> int:
    record = "--record" in sys.argv[1:]
    compiler = shutil.which("i686-w64-mingw32-gcc")
    if not compiler:
        print("wine mapvk skipped: needs i686-w64-mingw32-gcc")
        return 0
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        exe = work / "mapvk32.exe"
        subprocess.run([compiler, "-O1", "-Wall", "-Wextra", "-Werror", "-o", str(exe), str(SOURCE), "-luser32"],
                       check=True)

        if (DRIVER / "build/wine").exists() and (DRIVER / "prefix/system.reg").is_file():
            prefix = work / "prefix"
            shutil.copytree(DRIVER / "prefix", prefix, symlinks=True)
            frames = work / "frames"
            env = dict(os.environ, WINEDEBUG="-all")
            subprocess.run([str(ROOT / "tools/test_wine_ps5_driver.sh"), "--prefix", str(prefix), "--frames",
                            str(frames), "--seconds", "60", "--no-frame-check", str(exe)],
                           env=env, capture_output=True, text=True)
            check("drv", mapvk_line((frames / "run.log").read_text(errors="replace"), "drv"), record)
        else:
            print("wine mapvk drv skipped: needs .deps/wine-host-drv/build and its prefix")

        if os.environ.get("PW_TEST_WINEX11") == "1" and os.environ.get("DISPLAY") and DESKTOP.exists():
            prefix = WORK / "mapvk-prefix-desktop"
            env = dict(os.environ, WINEPREFIX=str(prefix), WINEARCH="wow64", WINEDEBUG="-all",
                       WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d")
            if not (prefix / "system.reg").is_file():
                prefix.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run([str(DESKTOP), "wineboot", "--init"], env=env, capture_output=True, timeout=300)
                subprocess.run([str(DESKTOP) + "server", "-w"], env=env, timeout=120)
            run = subprocess.run(["timeout", "-s", "KILL", "60", str(DESKTOP), str(exe)], env=env,
                                 capture_output=True, text=True)
            subprocess.run([str(DESKTOP) + "server", "-k"], env=env, capture_output=True)
            check("winex11", mapvk_line(run.stdout + run.stderr, "winex11"), record)
        else:
            print("wine mapvk winex11 skipped: needs PW_TEST_WINEX11=1, DISPLAY and .deps/wine-host-desktop")
    return 0


if __name__ == "__main__":
    sys.exit(main())
