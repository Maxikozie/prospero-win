#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tests/wine_d3d11tri64.c under DXVK 2.6.2 on the host, the way the PS4
runs it for milestone e: the host Wine with the PS4 user driver
(.deps/wine-host-drv/build: win32u rebuilt -DWINE_PS5_USER_DRIVER
-DWINE_PS5_HEADLESS_SURFACE, so the swapchain's surface is patch 0461's
headless one) through tools/test_wine_ps5_driver.sh, a 1920x1080 desktop,
DXVK's x64 d3d11.dll and dxgi.dll in a copy of its prefix with
WINEDLLOVERRIDES=d3d11,dxgi=n, on lavapipe. The line must report 600 frames,
no failing call, and the readback's centre yellow (ffff00) and 240,135 blue
(0000ff): the colours check.sh e expects from the console.

Needs that build tree and its prefix, lavapipe's ICD (PW_VK_ICD, default
/usr/share/vulkan/icd.d/lvp_icd.json), DXVK's release archive (PW_DXVK_ARCHIVE:
dxvk-2.6.2.tar.gz, checked against tools/pw_install.py's pin) and
x86_64-w64-mingw32-gcc; without any of them the check is skipped with a
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

SOURCE = ROOT / "tests" / "wine_d3d11tri64.c"
DRIVER = ROOT / ".deps/wine-host-drv"
LINE = re.compile(r"^D3D11 frames=600 swapchain=1920x1080 level=b000 hr=0x0 at=- px_c=ffff00 px_m=0000ff "
                  r"ms_avg=[0-9.]+$")
VERSION = "2.6.2"


def main() -> int:
    icd = os.environ.get("PW_VK_ICD", "/usr/share/vulkan/icd.d/lvp_icd.json")
    archive = Path(os.environ.get("PW_DXVK_ARCHIVE", ""))
    compiler = shutil.which("x86_64-w64-mingw32-gcc")
    missing = [what for what, ok in (
        ("the user-driver Wine (.deps/wine-host-drv/build)", (DRIVER / "build/wine").exists()),
        ("its prefix (.deps/wine-host-drv/prefix)", (DRIVER / "prefix/system.reg").is_file()),
        ("lavapipe", os.path.isfile(icd)),
        ("PW_DXVK_ARCHIVE", archive.is_file()),
        ("x86_64-w64-mingw32-gcc", bool(compiler))) if not ok]
    if missing:
        print(f"wine d3d11tri64 skipped: needs {', '.join(missing)}")
        return 0
    sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    assert sha == DXVK_RELEASES[VERSION][1], f"{archive} is not DXVK {VERSION} ({sha})"
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        exe = work / "d3d11tri64.exe"
        subprocess.run([compiler, "-O1", "-Wall", "-Wextra", "-Werror", "-o", str(exe), str(SOURCE),
                        "-ld3d11", "-ldxgi", "-ld3dcompiler_47", "-luser32"], check=True)
        prefix = work / "prefix"
        shutil.copytree(DRIVER / "prefix", prefix, symlinks=True)
        with tarfile.open(archive) as release:
            for dll in ("d3d11", "dxgi"):
                member = release.extractfile(f"dxvk-{VERSION}/x64/{dll}.dll")
                (prefix / "drive_c/windows/system32" / f"{dll}.dll").write_bytes(member.read())
        frames = work / "frames"
        env = dict(os.environ, WINEDLLOVERRIDES="d3d11,dxgi=n", WINE_PS5_DESKTOP="1920x1080",
                   VK_ICD_FILENAMES=icd, VK_DRIVER_FILES=icd, WINEDEBUG="-all")
        subprocess.run([str(ROOT / "tools/test_wine_ps5_driver.sh"), "--prefix", str(prefix), "--frames", str(frames),
                        "--seconds", "180", str(exe)], env=env, capture_output=True, text=True)
        log = (frames / "run.log").read_text(errors="replace")
        lines = [line.strip() for line in log.splitlines() if line.startswith("D3D11 ")]
        assert len(lines) == 1, log[-3000:]
        assert LINE.match(lines[0]), lines[0]
    print(f"wine d3d11tri64 passed: {lines[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
