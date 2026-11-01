#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Window callbacks on new threads (tests/wine_wincb.c), 32- and 64-bit, under
the host Wine: the PS4 port's phase p5 step "p5w" on the host.

1. The installed host Wine (headless, wow64cpu for 32-bit code): every
   HWND_MESSAGE case makes its window on the main thread and on new
   threads, the probes pass (direct call, a CBT hook on each thread, a send
   from a new thread to the main thread's window, 8 threads with a window
   each), and a hidden top-level window does the same on both threads (it
   fails on both in a Wine without a display driver).
2. The user-driver Wine (.deps/wine-host-drv, the console's win32u, 0910
   included) on the translator: every case and probe passes.
3. The same with tests/lab/pw_foreign_stack.c preloaded, which moves every
   new thread off the stack Wine gives it onto one of its own below Wine's
   kernel stack, as the PS4 title's pthread_create does. With patch 1100
   (ntdll's signal_init_thread_frame) every case and probe passes; with its
   switch WINE_PS5_THREAD_FRAME=0, or an ntdll without it, the PS4's
   failure is what must show: no new thread makes a window and their CBT
   hook is never called, while the main thread's windows, the direct call
   and the cross-thread send work (with 1100, KeUserModeCallback also logs
   the refusal once, and the first thread found off its kernel stack says
   whether its frame was moved or left in place).
4. The shim at 512 KiB (PW_FOREIGN_STACK_KIB), the stack the PS4 layer's
   "small" policy gives a caller that set its own: with 1100 every case and
   probe passes. Part 2 (no shim) is the layer's "keep" policy, the thread
   on the stack Wine gives it.

Every run makes its 8 hold threads twice (the second 8 after the first have
ended): both rounds must make their 8 windows.

Needs i686-w64-mingw32-gcc, x86_64-w64-mingw32-gcc, cc and the installed host
Wine (.deps/wine-host/install); parts 2 and 3 need .deps/wine-host-drv with
wowprospero (tools/refresh_wine_host_drv.sh) and the host build tree (for
the CPU key). Each part is skipped with a message when what it needs is
missing."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".deps/wine-host/build"
INSTALL = ROOT / ".deps/wine-host/install/usr/bin/wine"
DRIVER = ROOT / ".deps/wine-host-drv"
CPU_KEY = r"HKLM\Software\Microsoft\Wow64\x86"
CFLAGS = ["-O1", "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror"]
CASES = 23


def wine_env(prefix: Path, **extra: str) -> dict:
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG="-all", WINEDLLOVERRIDES="winemenubuilder.exe=d")
    env.update(extra)
    return env


def server(wine: Path, prefix: Path, flag: str) -> None:
    path = wine.parent / ("server/wineserver" if (wine.parent / "server").is_dir() else "wineserver")
    subprocess.run([str(path), flag], env=wine_env(prefix), check=False)


def parse(text: str) -> tuple[dict, dict, dict]:
    """The case lines by number, the probe lines by name and thread, the done line's fields."""
    cases, probes, done = {}, {}, {}
    for line in text.splitlines():
        if not line.startswith("WINCB "):
            continue
        fields = dict(re.findall(r"(\w+)=(\S+)", line))
        if "case" in fields:
            cases[int(fields["case"])] = fields
        elif "probe" in fields:
            probes[fields["probe"] + ("_" + fields["thread"] if "thread" in fields else "")] = fields
        elif line.startswith("WINCB done "):
            done = fields
    return cases, probes, done


def check_all_pass(text: str, what: str) -> None:
    cases, probes, done = parse(text)
    assert len(cases) == CASES and done, f"{what}: {len(cases)} cases, done={done}\n{text[-3000:]}"
    bad = [n for n, c in cases.items() if c["hwnd"] != "ok" or c.get("second", "-") == "0"]
    assert not bad, f"{what}: cases {bad} failed: {[cases[n] for n in bad]}"
    own = [c for c in cases.values() if c["class"] != "builtin"]
    assert all(c["nc_cs"] == "ok" and c["msgs"].startswith("24,81") for c in own), f"{what}: {own}"
    assert all(c["def_ret"] == "1" for c in own if c["class"] == "owndef"), f"{what}: def_ret"
    assert done["direct_main"] == done["direct_new"] == "1", f"{what}: {done}"
    assert done["cbt_main"] == done["cbt_new"] == "1" and done["cross"] == "ok", f"{what}: {done}"
    assert done["hold_windows"] == done["hold2_windows"] == "8", f"{what}: {done}"
    assert re.search(r"^WINCB hold2 n=8 alive=8 windows=8$", text, re.M), f"{what}: no second hold line"


def check_ps4_failure(text: str, what: str) -> None:
    """The PS4's pattern: windows on the main thread only, no callback on a new thread."""
    cases, probes, done = parse(text)
    assert len(cases) == CASES and done, f"{what}: {len(cases)} cases, done={done}"
    main = [c for c in cases.values() if c["thread"] == "main"]
    new = [c for c in cases.values() if c["thread"] != "main"]
    assert all(c["hwnd"] == "ok" for c in main), f"{what}: main thread: {main}"
    assert all(c["hwnd"] == "0" and c["msgs"] == "-" for c in new), f"{what}: new threads: {new}"
    assert done["direct_new"] == "1" and done["cbt_new"] == "0" and done["cross"] == "ok", f"{what}: {done}"
    assert done["hold_windows"] == done["hold2_windows"] == "0", f"{what}: {done}"


def run_installed(exe: Path, prefix: Path) -> str:
    done = subprocess.run([str(INSTALL), str(exe)], env=wine_env(prefix), capture_output=True, text=True,
                          timeout=600)
    server(INSTALL, prefix, "-k")
    assert done.returncode == 0, f"{exe.name}: exit {done.returncode}: {done.stdout}{done.stderr[-2000:]}"
    return done.stdout


def run_driver(exe: Path, prefix: Path, frames: Path, shim: Path | None, **extra: str) -> str:
    args = [str(ROOT / "tools/test_wine_ps5_driver.sh"), "--prefix", str(prefix), "--frames", str(frames),
            "--seconds", "180", "--no-frame-check"]
    if shim:
        args += ["--preload", str(shim)]
    subprocess.run(args + [str(exe)], env=wine_env(prefix, WINEDEBUG="-all,err+seh", **extra), capture_output=True,
                   text=True, check=False)
    return (frames / "run.log").read_text(errors="replace") if (frames / "run.log").exists() else ""


def main() -> int:
    i686, x64, cc = (shutil.which(t) for t in ("i686-w64-mingw32-gcc", "x86_64-w64-mingw32-gcc", "cc"))
    missing = [what for what, ok in (("i686-w64-mingw32-gcc", i686), ("x86_64-w64-mingw32-gcc", x64), ("cc", cc),
                                     ("the installed host Wine", os.access(INSTALL, os.X_OK))) if not ok]
    if missing:
        print("wine wincb skipped: needs " + ", ".join(missing))
        return 0
    with tempfile.TemporaryDirectory(prefix="pw-wincb-") as directory:
        work = Path(directory)
        exes = {32: work / "wincb32.exe", 64: work / "wincb64.exe"}
        for bits, compiler in ((32, i686), (64, x64)):
            subprocess.run([compiler, *CFLAGS, "-o", str(exes[bits]), str(ROOT / "tests/wine_wincb.c"), "-luser32"],
                           check=True)
        shim = work / "pw_foreign_stack.so"
        subprocess.run([cc, "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror", "-o", str(shim),
                        str(ROOT / "tests/lab/pw_foreign_stack.c"), "-ldl", "-lpthread"], check=True)

        # 1. The installed Wine.
        native = work / "native"
        subprocess.run([str(INSTALL), "wineboot", "--init"], env=wine_env(native), capture_output=True,
                       timeout=600, check=False)
        server(INSTALL, native, "-w")
        assert (native / "system.reg").is_file(), "wineboot made no prefix"
        for bits, exe in exes.items():
            text = run_installed(exe, native)
            cases, probes, done = parse(text)
            assert len(cases) == CASES and done, f"installed {bits}: {text}"
            bad = [n for n, c in cases.items() if c["parent"] == "msg" and c["hwnd"] != "ok"]
            assert not bad, f"installed {bits}: HWND_MESSAGE cases {bad} failed:\n{text}"
            top = {(c["class"], c["cs"]): set() for c in cases.values() if c["parent"] == "top"}
            for c in cases.values():
                if c["parent"] == "top":
                    top[(c["class"], c["cs"])].add(c["hwnd"])
            assert all(len(v) == 1 for v in top.values()), f"installed {bits}: top-level differs by thread: {top}"
            assert done["direct_main"] == done["direct_new"] == "1" and done["cross"] == "ok", done
            assert done["cbt_main"] == done["cbt_new"] == "1" and done["hold_windows"] == "8", done
            assert done["hold2_windows"] == "8", done
            print(f"wine wincb: {bits}-bit on the installed Wine: every HWND_MESSAGE case on every thread, "
                  f"top-level windows {'made' if 'ok' in next(iter(top.values())) else 'refused'} on both threads")

        # 2. and 3. The user-driver Wine on the translator, then with threads off their stack.
        if not ((DRIVER / "build/dlls/wowprospero/wowprospero.so").is_file() and (BUILD / "wine").exists()):
            print("wine wincb: the driver parts skipped: needs .deps/wine-host-drv with wowprospero "
                  "(tools/refresh_wine_host_drv.sh) and the host build tree")
            return 0
        prefix = work / "drv-prefix"
        shutil.copytree(native, prefix, symlinks=True)
        subprocess.run([str(BUILD / "wine"), "reg", "add", CPU_KEY, "/ve", "/d", "wowprospero.dll", "/f"],
                       env=wine_env(prefix), capture_output=True, check=True, timeout=300)
        server(BUILD / "wine", prefix, "-w")
        symbols = subprocess.run(["nm", "-D", "--defined-only", str(DRIVER / "build/dlls/ntdll/ntdll.so")],
                                 capture_output=True, text=True, check=False).stdout
        symbols += subprocess.run(["nm", str(DRIVER / "build/dlls/ntdll/ntdll.so")], capture_output=True, text=True,
                                  check=False).stdout
        fixed = "signal_init_thread_frame" in symbols
        for bits, exe in exes.items():
            text = run_driver(exe, prefix, work / f"frames-{bits}", None)
            check_all_pass(text, f"driver {bits}")
            assert "refused" not in text, f"driver {bits}: a callback was refused"
            text = run_driver(exe, prefix, work / f"frames-{bits}-shim", shim)
            moved = re.search(r"pw_foreign_stack: (\d+) thread\(s\) moved off their stack, (\d+) of them above", text)
            assert moved and int(moved.group(1)) >= 25 and moved.group(2) == "0", \
                f"driver {bits} + shim: the shim did not move the threads below their stack: {moved}"
            if fixed:
                check_all_pass(text, f"driver {bits} + shim + 1100")
                assert "refused" not in text, f"driver {bits} + shim: a callback was refused"
                assert re.search(r"err:seh:signal_init_thread_frame thread frame moved to the kernel stack "
                                 r"\(stack 0x\w+, kernel stack 0x\w+\)", text), f"driver {bits}: no 'moved' line"
                # 1100's switch keeps the old placement: the PS4's failure, and its one ERR line.
                text = run_driver(exe, prefix, work / f"frames-{bits}-shim-off", shim, WINE_PS5_THREAD_FRAME="0")
                check_ps4_failure(text, f"driver {bits} + shim + WINE_PS5_THREAD_FRAME=0")
                assert re.search(r"err:seh:KeUserModeCallback callback \d+ refused: stack 0x\w+ is not above the "
                                 r"kernel stack 0x\w+\+0x2000", text), f"driver {bits}: no refusal line:\n{text[-2000:]}"
                assert re.search(r"err:seh:signal_init_thread_frame thread frame left in place \(switch off; "
                                 r"stack 0x\w+, kernel stack 0x\w+\)", text), f"driver {bits}: no 'left' line"
                # 4. The small policy's 512 KiB stack: the frame moved, every case.
                text = run_driver(exe, prefix, work / f"frames-{bits}-shim-small", shim, PW_FOREIGN_STACK_KIB="512")
                assert re.search(r"pw_foreign_stack: moved a thread off its stack 0x\w+\+0x\w+ onto 512 KiB", text), \
                    f"driver {bits} + shim at 512 KiB: no 512 KiB move:\n{text[-2000:]}"
                check_all_pass(text, f"driver {bits} + shim at 512 KiB + 1100")
                assert "refused" not in text, f"driver {bits} + shim at 512 KiB: a callback was refused"
            else:
                check_ps4_failure(text, f"driver {bits} + shim, ntdll without 1100")
            print(f"wine wincb: {bits}-bit on the user driver and the translator: every case; with "
                  f"{moved.group(1)} threads off their stack: "
                  + ("every case (patch 1100, also on 512 KiB stacks), and the PS4's failure with "
                     "WINE_PS5_THREAD_FRAME=0" if fixed
                     else "the PS4's failure (ntdll without patch 1100)"))
    print("wine wincb passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
