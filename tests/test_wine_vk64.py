#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tests/wine_vk64.c under the host Wine on lavapipe: the first line, the
instance, device and submit through winevulkan. The plain host Wine has no
display driver, so the second line must be the window's failure; the e3 half
(win32u's PS4 driver, patch 0461's headless surface, a 1920x1080 swapchain,
330 frames) runs under the user-driver build:
  WINE_PS5_DESKTOP=1920x1080 VK_ICD_FILENAMES=<lavapipe> \
      tools/test_wine_ps5_driver.sh --seconds 90 vk64.exe
(.deps/wine-host-drv/build: the host tree with win32u rebuilt
-DWINE_PS5_USER_DRIVER -DWINE_PS5_HEADLESS_SURFACE). The PS4 runs the same
program for milestones e2 and e3. Whether every extension DXVK needs is
offered depends on the host driver, so exts_dxvk is printed, not checked.

Needs the host Wine built with Vulkan (tools/build_host_wine.sh with
ac_cv_lib_soname_vulkan=libvulkan.so.1; PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine), lavapipe's ICD
(/usr/share/vulkan/icd.d/lvp_icd.json, or PW_VK_ICD), the Khronos headers
(PW_VULKAN_INCLUDE: the directory holding vulkan/vulkan.h, mesa-ps4's
include/) and x86_64-w64-mingw32-gcc; without any of them the check is
skipped with a message."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_vk64.c"
FIRST = re.compile(r"^VK64 inst=ok api=1\.\d+ device=\S+ exts=\d+ exts_dxvk=\S+ dev=ok submit=ok$")
SECOND = re.compile(r"^VK64S surface=fail:window:\d+$")


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    config = ROOT / ".deps/wine-host/build/include/config.h"
    icd = os.environ.get("PW_VK_ICD", "/usr/share/vulkan/icd.d/lvp_icd.json")
    include = os.environ.get("PW_VULKAN_INCLUDE", "")
    compiler = shutil.which("x86_64-w64-mingw32-gcc")
    missing = [what for what, ok in (
        ("the host Wine", os.access(wine, os.X_OK)),
        ("a host Wine built with Vulkan", config.is_file() and "#define SONAME_LIBVULKAN" in config.read_text()),
        ("lavapipe", os.path.isfile(icd)),
        ("PW_VULKAN_INCLUDE", bool(include) and (Path(include) / "vulkan/vulkan.h").is_file()),
        ("x86_64-w64-mingw32-gcc", bool(compiler))) if not ok]
    if missing:
        print(f"wine vk64 skipped: needs {', '.join(missing)}")
        return 0
    with tempfile.TemporaryDirectory() as directory:
        exe = Path(directory) / "vk64.exe"
        subprocess.run([compiler, "-O1", "-Wall", "-Wextra", "-Werror", "-idirafter", include, "-o", str(exe),
                        str(SOURCE), "-luser32"], check=True)
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="winemenubuilder.exe=d", VK_ICD_FILENAMES=icd, VK_DRIVER_FILES=icd)
        done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=env, timeout=600)
        lines = [line.strip() for line in done.stdout.splitlines() if line.startswith("VK64")]
        assert len(lines) == 2, f"exit {done.returncode}: {done.stdout}{done.stderr[-2000:]}"
        assert FIRST.match(lines[0]), lines[0]
        assert SECOND.match(lines[1]), lines[1]
    print(f"wine vk64 passed: {lines[0]} / {lines[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
