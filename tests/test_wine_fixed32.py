#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tests/wine_fixed32.c, a PE32 linked at 0x400000 without a relocation
section and with SizeOfImage past 0x40c000, under the host Wine's WoW64: it
loads at its base, its sections keep their protections (code RX, a write to a
read-only section that fills a 16 KiB host page of its own faults, .data and
.bss write), a frame handler catches a fault and a second thread runs. With
the PC's 4 KiB host pages and with WINE_HOST_PAGE_SIZE=16384 (the console's 16
KiB pages, where .text, .data and .rdata share one host page). A write to that
shared .rdata (rdata_shared) faults with 4 KiB pages and goes through with 16
KiB ones: patch 0974 gives an image's host page the union of its pages'
protections, by design. The PS4 port's milestone i runs the same program
on the console after the title released its eboot.

The image itself is checked first: ImageBase 0x400000, relocations stripped,
no relocation directory, SizeOfImage >= 0x40c000.

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and i686-w64-mingw32-gcc; without them
the check is skipped with a message."""
from __future__ import annotations

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_fixed32.c"
CFLAGS = ["-O1", "-s", "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror",
          "-Wl,--image-base,0x400000,--disable-dynamicbase,--disable-reloc-section"]
LINE = re.compile(r"^FIXED32 bits=(?P<bits>\S+) base=(?P<base>\S+) relocs=(?P<relocs>\S+) size_kib=(?P<size_kib>\d+) "
                  r"text_rx=(?P<text_rx>\S+) rdata_ro=(?P<rdata_ro>\S+) rdata_shared=(?P<rdata_shared>\S+) "
                  r"data_rw=(?P<data_rw>\S+) seh=(?P<seh>\S+) "
                  r"thread=(?P<thread>\S+)$")


def image(exe: Path) -> tuple[int, int, int, int]:
    """ImageBase, Characteristics, the relocation directory's size, SizeOfImage."""
    data = exe.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0", "not a PE"
    characteristics = struct.unpack_from("<H", data, pe + 22)[0]
    optional = pe + 24
    assert struct.unpack_from("<H", data, optional)[0] == 0x10B, "not PE32"
    base = struct.unpack_from("<I", data, optional + 28)[0]
    size = struct.unpack_from("<I", data, optional + 56)[0]
    reloc_size = struct.unpack_from("<I", data, optional + 96 + 5 * 8 + 4)[0]
    return base, characteristics, reloc_size, size


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compiler = shutil.which("i686-w64-mingw32-gcc")
    if not os.access(wine, os.X_OK) or not compiler:
        print("wine fixed32 skipped: needs the host Wine and i686-w64-mingw32-gcc")
        return 0
    with tempfile.TemporaryDirectory() as directory:
        exe = Path(directory) / "fixed32.exe"
        subprocess.run([compiler, *CFLAGS, "-o", str(exe), str(SOURCE)], check=True)
        base, characteristics, reloc_size, size = image(exe)
        assert base == 0x400000, f"ImageBase 0x{base:x}"
        assert characteristics & 0x0001, "relocations not stripped"
        assert reloc_size == 0, f"a relocation directory of {reloc_size} bytes"
        assert size >= 0x40C000, f"SizeOfImage 0x{size:x}"
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="winemenubuilder.exe=d")
        for host_page in ("", "16384"):
            run_env = dict(env, WINE_HOST_PAGE_SIZE=host_page) if host_page else env
            label = f"host pages {host_page or 'native'}"
            done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=run_env, timeout=300)
            lines = [line.strip() for line in done.stdout.splitlines() if line.startswith("FIXED32 ")]
            assert len(lines) == 1, f"{label}: exit {done.returncode}: {done.stdout}{done.stderr[-2000:]}"
            match = LINE.match(lines[0])
            assert match, f"{label}: {lines[0]}"
            got = match.groupdict()
            want = dict(bits="32", base="0x00400000", relocs="0", text_rx="ok", rdata_ro="ok", data_rw="ok",
                        seh="ok", thread="ok", rdata_shared="writable" if host_page else "faults")
            wrong = {key: got[key] for key, value in want.items() if got[key] != value}
            assert not wrong and int(got["size_kib"]) >= 4144, f"{label}: {lines[0]}"
            assert done.returncode == 0, f"{label}: exit {done.returncode}"
            print(f"{label}: {lines[0]}")
    print("wine fixed32 passed: a PE32 fixed at 0x400000 loads there with its sections' protections, "
          "4 KiB and 16 KiB host pages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
