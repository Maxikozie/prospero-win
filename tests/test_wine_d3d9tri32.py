#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tests/wine_d3d9tri32.c under DXVK 2.6.2's 32-bit d3d9 on the host, the way
the PS4 runs it for milestone g's g1: the host Wine with the PS4 user driver
(.deps/wine-host-drv/build, patch 0461's headless surface) through
tools/test_wine_ps5_driver.sh, a 1920x1080 desktop, new WoW64, DXVK's x32
d3d9.dll in a copy of its prefix's syswow64 with WINEDLLOVERRIDES=d3d9=n,
the three settings of DXVK's built-in vampire.exe profile in C:\\g1\\dxvk.conf
(DXVK_CONFIG_FILE), Vulkan batching on (PW_VK_BATCH=1, PW_VK_BATCH_STATS=1),
on lavapipe. The line must report 32 bits, 600 frames, no failing call, and
the readback's centre yellow (ffff00) and 240,135 green (00ff00, the MANAGED
texture): the colours check.sh g1 expects from the console. DXVK must report
the config file, and the last PW_VK_BATCH record must say enabled=1
negotiated=1.

Needs that build tree and its prefix, lavapipe's ICD (PW_VK_ICD, default
/usr/share/vulkan/icd.d/lvp_icd.json), DXVK's release archive (PW_DXVK_ARCHIVE:
dxvk-2.6.2.tar.gz, checked against tools/pw_install.py's pin) and
i686-w64-mingw32-gcc; without any of them the check is skipped with a
message."""
from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from pw_install import DXVK_RELEASES  # noqa: E402

SOURCE = ROOT / "tests" / "wine_d3d9tri32.c"
DRIVER = ROOT / ".deps/wine-host-drv"
LINE = re.compile(r"^D3D9 bits=32 frames=600 swapchain=1920x1080 hr=0x0 at=- px_c=ffff00 px_m=00ff00 "
                  r"texmem_mib=[0-9]+ ms_avg=[0-9.]+$")
BATCH = re.compile(r"PW_VK_BATCH version=1 .* enabled=1 negotiated=1 records=[1-9]")
# DXVK 2.6.2's src/util/config/config.cpp entry for \\vampire\.exe$ (docs/ps4/HANDOFF-g.md in ps4-wine).
VAMPIRE = "d3d9.deferSurfaceCreation = True\nd3d9.memoryTrackTest = True\nd3d9.maxAvailableMemory = 1024\n"
VERSION = "2.6.2"


def main() -> int:
    icd = os.environ.get("PW_VK_ICD", "/usr/share/vulkan/icd.d/lvp_icd.json")
    archive = Path(os.environ.get("PW_DXVK_ARCHIVE", ""))
    compiler = shutil.which("i686-w64-mingw32-gcc")
    missing = [what for what, ok in (
        ("the user-driver Wine (.deps/wine-host-drv/build)", (DRIVER / "build/wine").exists()),
        ("its prefix (.deps/wine-host-drv/prefix)", (DRIVER / "prefix/system.reg").is_file()),
        ("lavapipe", os.path.isfile(icd)),
        ("PW_DXVK_ARCHIVE", archive.is_file()),
        ("i686-w64-mingw32-gcc", bool(compiler))) if not ok]
    if missing:
        print(f"wine d3d9tri32 skipped: needs {', '.join(missing)}")
        return 0
    sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    assert sha == DXVK_RELEASES[VERSION][1], f"{archive} is not DXVK {VERSION} ({sha})"
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        exe = work / "d3d9tri32.exe"
        subprocess.run([compiler, "-O1", "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror", "-o", str(exe),
                        str(SOURCE), "-ld3d9", "-luser32"], check=True)
        prefix = work / "prefix"
        shutil.copytree(DRIVER / "prefix", prefix, symlinks=True)
        with tarfile.open(archive) as release:
            member = release.extractfile(f"dxvk-{VERSION}/x32/d3d9.dll")
            (prefix / "drive_c/windows/syswow64/d3d9.dll").write_bytes(member.read())
        (prefix / "drive_c/g1").mkdir(exist_ok=True)
        (prefix / "drive_c/g1/dxvk.conf").write_text(VAMPIRE)
        frames = work / "frames"
        env = dict(os.environ, WINEDLLOVERRIDES="d3d9=n", WINE_PS5_DESKTOP="1920x1080", VK_ICD_FILENAMES=icd,
                   VK_DRIVER_FILES=icd, WINEDEBUG="-all", DXVK_CONFIG_FILE="C:\\g1\\dxvk.conf", PW_VK_BATCH="1",
                   PW_VK_BATCH_STATS="1")
        subprocess.run([str(ROOT / "tools/test_wine_ps5_driver.sh"), "--prefix", str(prefix), "--frames", str(frames),
                        "--seconds", "180", str(exe)], env=env, capture_output=True, text=True)
        log = (frames / "run.log").read_text(errors="replace")
        lines = [line.strip() for line in log.splitlines() if line.startswith("D3D9 ")]
        assert len(lines) == 1, log[-3000:]
        assert LINE.match(lines[0]), lines[0]
        assert "Found config file: C:\\g1\\dxvk.conf" in log, "DXVK did not read C:\\g1\\dxvk.conf"
        batch = [line for line in log.splitlines() if "PW_VK_BATCH version=1" in line]
        assert batch and BATCH.search(batch[-1]), batch[-1:] or "no PW_VK_BATCH record"
    print(f"wine d3d9tri32 passed: {lines[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
