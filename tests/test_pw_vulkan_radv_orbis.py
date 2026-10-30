#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""libvulkan.prx's glue (wine/orbis/pw_vulkan_radv_orbis.c) on the host,
against tests/pw_vulkan_radv_orbis_fake.c's fake driver, plain and with
sanitizers, when a Vulkan 1.3 header is found (PW_VULKAN_INCLUDE, e.g.
mesa-ps4's include/, or the system's); skipped otherwise."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER_VERSION = re.compile(r"^#define VK_HEADER_VERSION (\d+)$", re.M)
MIN_HEADER_VERSION = 204  # VkSubmitInfo2 (Vulkan 1.3)


def vulkan_include() -> Path | None:
    named = os.environ.get("PW_VULKAN_INCLUDE")
    for candidate in ([Path(named)] if named else []) + [Path("/usr/include")]:
        core = candidate / "vulkan/vulkan_core.h"
        if not (candidate / "vulkan/vulkan.h").is_file() or not core.is_file():
            continue
        match = HEADER_VERSION.search(core.read_text())
        if match and int(match[1]) >= MIN_HEADER_VERSION:
            return candidate
    return None


def main() -> int:
    include = vulkan_include()
    if not include:
        print(f"libvulkan.prx glue: no Vulkan header {MIN_HEADER_VERSION} or newer (PW_VULKAN_INCLUDE); skipped")
        return 0
    cc = shutil.which("cc")
    assert cc
    flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'wine/orbis'}", f"-I{include}"]
    with tempfile.TemporaryDirectory(prefix="pw-vulkan-radv-orbis-") as directory:
        out = Path(directory)
        for name, extra in [("host", ["-O2"]),
                            ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]:
            subprocess.run([cc, *flags, *extra, str(ROOT / "wine/orbis/pw_vulkan_radv_orbis.c"),
                            str(ROOT / "tests/pw_vulkan_radv_orbis_fake.c"), "-o", str(out / name)], check=True)
            run = subprocess.run([str(out / name)], capture_output=True, text=True, check=True)
            assert "PASS" in run.stdout, run.stdout
    print(f"libvulkan.prx glue: wrappers and stats pass against {include}/vulkan")
    return 0


if __name__ == "__main__":
    sys.exit(main())
