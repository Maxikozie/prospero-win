#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""32-bit programs under the host Wine's WoW64 on prospero-win's CPU backend,
wowprospero (the IA-32 translator): the PS4 port's milestones f1 and f2 on
the host.

1. tests/wine_hello32.c passes every check (HELLO32 fp=ok thread=ok,
   HELLO32S seh=ok, "Hello from PE32", exit 0) with the prefix's
   Wow64\\x86 key naming wowprospero.dll, which WINEDEBUG=+loaddll must show
   loaded; and on Wine's own wow64cpu.dll as the control, so the expected
   values are the processor's.
2. The same on a wowprospero built -DPW_X86_NO_BMI2 (the encodings of the
   PS4's Jaguar, which has no BMI2), from a separate directory through
   WINEDLLPATH under the installed host Wine.
3. tests/wine_wow32.c (the console's one program for f1 + f2: the checks,
   then gdi64.exe's picture built for i686) under the user-driver Wine
   (tools/test_wine_ps5_driver.sh, .deps/wine-host-drv) on the translator:
   the checks pass and a presented frame holds the window, orange 400x300
   at 200,150 of the 800x600 desktop with the green 200x150 inside, as the
   title shows it (check.sh f2's pixels).

Needs the host Wine build tree with wowprospero (tools/build_wowprospero.sh
into .deps/wine-host/build; PROSPERO_WINE_BUILD and PROSPERO_WINE_SOURCE
override), the installed host Wine (.deps/wine-host/install, which makes the
prefix) and i686-w64-mingw32-gcc; part 3 needs .deps/wine-host-drv with
wowprospero (tools/refresh_wine_host_drv.sh). Each part is skipped with a
message when what it needs is missing."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("PROSPERO_WINE_BUILD", ROOT / ".deps/wine-host/build"))
SOURCE = Path(os.environ.get("PROSPERO_WINE_SOURCE", ROOT / ".deps/wine-host/source"))
INSTALL = ROOT / ".deps/wine-host/install/usr/bin/wine"
DRIVER = ROOT / ".deps/wine-host-drv"
CPU_KEY = r"HKLM\Software\Microsoft\Wow64\x86"
CFLAGS = ["-O1", "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror"]
HELLO_LINES = ["HELLO32 fp=ok thread=ok", "HELLO32S seh=ok", "Hello from PE32"]


def wine_env(prefix: Path, **extra: str) -> dict:
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG="-all", WINEDLLOVERRIDES="winemenubuilder.exe=d")
    env.update(extra)
    return env


def server_wait(wine: Path, prefix: Path, kill: bool = False) -> None:
    server = wine.parent / ("server/wineserver" if wine.name == "wine" and (wine.parent / "server").is_dir()
                            else "wineserver")
    subprocess.run([str(server), "-k" if kill else "-w"], env=wine_env(prefix), check=False)


def set_cpu(wine: Path, prefix: Path) -> None:
    subprocess.run([str(wine), "reg", "add", CPU_KEY, "/ve", "/d", "wowprospero.dll", "/f"],
                   env=wine_env(prefix), capture_output=True, check=True, timeout=300)
    server_wait(wine, prefix)


def hello(wine: Path, prefix: Path, exe: Path, what: str, **extra: str) -> str:
    """Runs hello32.exe; asserts its lines and exit 0; returns stderr (loaddll)."""
    done = subprocess.run([str(wine), str(exe)], env=wine_env(prefix, **extra), capture_output=True,
                          text=True, timeout=300)
    server_wait(wine, prefix, kill=True)
    lines = [line for line in done.stdout.splitlines() if line.startswith(("HELLO32", "Hello"))]
    assert done.returncode == 0 and lines == HELLO_LINES, f"{what}: exit {done.returncode}: {done.stdout}"
    return done.stderr


def loaded(stderr: str, name: str) -> list[str]:
    return [line for line in stderr.splitlines() if "Loaded" in line and name in line]


def window_frame(frames: Path) -> Path | None:
    """The first presented frame (P6 PPM) holding gdi64.exe's window."""
    for ppm in sorted(frames.glob("[0-9]*.ppm")):
        data = ppm.read_bytes()
        fields, at = [], 0
        while len(fields) < 4:
            while data[at:at + 1].isspace():
                at += 1
            if data[at:at + 1] == b"#":
                at = data.index(b"\n", at)
                continue
            end = at
            while not data[end:end + 1].isspace():
                end += 1
            fields.append(data[at:end])
            at = end
        at += 1
        width = int(fields[1])

        def rgb(x: int, y: int) -> tuple:
            o = at + (y * width + x) * 3
            return tuple(data[o:o + 3])
        if fields[0] == b"P6" and rgb(220, 170) == (255, 128, 0) and rgb(400, 300) == (0, 255, 0):
            return ppm
    return None


def main() -> int:
    compiler = shutil.which("i686-w64-mingw32-gcc")
    unix = BUILD / "dlls/wowprospero/wowprospero.so"
    missing = [what for what, ok in (("i686-w64-mingw32-gcc", compiler), ("the host Wine build tree", (BUILD / "wine").exists()),
                                     ("wowprospero in it (tools/build_wowprospero.sh)", unix.is_file()),
                                     ("the installed host Wine", os.access(INSTALL, os.X_OK))) if not ok]
    if missing:
        print("wine wow32 skipped: needs " + ", ".join(missing))
        return 0
    with tempfile.TemporaryDirectory(prefix="pw-wow32-") as directory:
        work = Path(directory)
        hello32, wow32 = work / "hello32.exe", work / "wow32.exe"
        subprocess.run([compiler, *CFLAGS, "-o", str(hello32), str(ROOT / "tests/wine_hello32.c")], check=True)
        subprocess.run([compiler, *CFLAGS, "-o", str(wow32), str(ROOT / "tests/wine_wow32.c"), "-lgdi32", "-luser32"],
                       check=True)

        # 1. A prefix from the installed Wine; the control on wow64cpu, then the translator.
        native = work / "native"
        subprocess.run([str(INSTALL), "wineboot", "--init"], env=wine_env(native), capture_output=True,
                       timeout=600, check=False)
        server_wait(INSTALL, native)
        assert (native / "system.reg").is_file(), "wineboot made no prefix"
        dbt = work / "dbt"
        shutil.copytree(native, dbt, symlinks=True)
        hello(INSTALL, native, hello32, "control (wow64cpu)")
        set_cpu(BUILD / "wine", dbt)
        stderr = hello(BUILD / "wine", dbt, hello32, "translator", WINEDEBUG="+loaddll")
        assert loaded(stderr, "wowprospero.dll"), "wowprospero.dll was not loaded"
        print("wine wow32: hello32.exe passed on wow64cpu (control) and on wowprospero")

        # 2. Jaguar's encodings: a -DPW_X86_NO_BMI2 build through WINEDLLPATH (a build-tree Wine
        # would take its own build directory's first).
        if (SOURCE / "dlls/wow64").is_dir():
            nobmi2 = work / "nobmi2"
            built = subprocess.run([str(ROOT / "tools/build_wowprospero.sh"), "--build", str(BUILD), "--source",
                                    str(SOURCE), "--output", str(nobmi2), "--cflags", "-DPW_X86_NO_BMI2"],
                                   capture_output=True, text=True, check=False)
            assert built.returncode == 0, built.stdout + built.stderr
            stderr = hello(INSTALL, dbt, hello32, "translator without BMI2", WINEDEBUG="+loaddll",
                           WINEDLLPATH=str(nobmi2))
            assert loaded(stderr, "wowprospero.dll"), "the -DPW_X86_NO_BMI2 wowprospero.dll was not loaded"
            print("wine wow32: hello32.exe passed on wowprospero built -DPW_X86_NO_BMI2 (Jaguar's encodings)")
        else:
            print(f"wine wow32: no-BMI2 run skipped: needs the Wine source at {SOURCE}")

        # 3. wow32.exe under the user driver: the checks, then the picture.
        drv_unix = DRIVER / "build/dlls/wowprospero/wowprospero.so"
        if not (drv_unix.is_file() and (DRIVER / "prefix/system.reg").is_file()):
            print("wine wow32: the driver part skipped: needs .deps/wine-host-drv with wowprospero "
                  "(tools/refresh_wine_host_drv.sh) and its prefix")
            return 0
        prefix, frames = work / "drv-prefix", work / "frames"
        shutil.copytree(DRIVER / "prefix", prefix, symlinks=True)
        set_cpu(BUILD / "wine", prefix)   # the plain build tree: wineboot-like tools hang under the driver
        subprocess.run([str(ROOT / "tools/test_wine_ps5_driver.sh"), "--prefix", str(prefix), "--frames", str(frames),
                        "--seconds", "90", str(wow32)], capture_output=True, text=True, check=False)
        log = (frames / "run.log").read_text(errors="replace") if (frames / "run.log").exists() else ""
        lines = [line for line in log.splitlines() if line.startswith(("HELLO32", "Hello", "GDI32"))]
        assert lines[:3] == HELLO_LINES, f"wow32.exe's checks under the driver: {lines}"
        gdi = [line for line in lines if line.startswith("GDI32 ")]
        assert gdi and " pixel=ff8000 " in gdi[0] and " window=ok " in gdi[0], f"GDI32 line: {gdi}"
        frame = window_frame(frames)
        assert frame, f"no presented frame holds the window ({len(list(frames.glob('[0-9]*.ppm')))} frames)"
        print(f"wine wow32 passed: wow32.exe on wowprospero under the user driver, {gdi[0].split(' fonts')[0]}, "
              f"the window in {frame.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
