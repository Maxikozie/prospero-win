#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tests/wine_xinput32.c (32-bit, new WoW64) on the host Wine with the PS4
user driver (.deps/wine-host-drv/build, through tools/test_wine_ps5_driver.sh,
in a copy of its prefix): xinput1_3.dll's title-pad path (patches 0470/0471)
as the PS4 port's milestone l1 runs it, with tests/fake_pad_sink.c preloaded
in place of ntdll.prx's pad slot. The sink scripts controller 0 through every
button of 0xf3ff, both extremes of each stick axis and both triggers at 255,
and logs the rumble it gets. The line must report get0=0 caps=0 get1=1167,
buttons_seen=0xf3ff, every axis at -32768 and 32767, lt_max=rt_max=255,
set0=0, rumble_events>=2 and bits=32, and the sink must have had at least
two rumble calls with a motor on.

A second run without the sink is the path everywhere but in a title: the
Unix library finds no pad slot, xinput falls back to HID, which has no
devices here: get0=1167 get1=1167.

Needs that build tree and its prefix, i686-w64-mingw32-gcc and cc; without
any of them the check is skipped with a message."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_xinput32.c"
SINK = ROOT / "tests" / "fake_pad_sink.c"
DRIVER = ROOT / ".deps/wine-host-drv"
SECONDS = "2"
FIELDS = ("bits get0 caps get1 vid pid buttons_seen lx_min lx_max ly_min ly_max rx_min rx_max ry_min ry_max "
          "lt_max rt_max packets set0 rumble_events seconds").split()
LINE = re.compile(r"^XINPUT " + " ".join(f"{name}=(?P<{name}>\\S+)" for name in FIELDS) + "$")


def xinput_line(log: str, what: str) -> tuple[str, dict[str, str]]:
    lines = [line.strip() for line in log.splitlines() if line.startswith("XINPUT ")]
    assert len(lines) == 1, f"{what}: no single XINPUT line:\n{log[-3000:]}"
    match = LINE.match(lines[0])
    assert match, f"{what}: {lines[0]}"
    return lines[0], match.groupdict()


def run(exe: Path, prefix: Path, frames: Path, preload: list[str]) -> str:
    args = [str(ROOT / "tools/test_wine_ps5_driver.sh"), "--prefix", str(prefix), "--frames", str(frames),
            "--seconds", "90", "--no-frame-check"]
    for library in preload:
        args += ["--preload", library]
    subprocess.run(args + [str(exe), SECONDS], env=dict(os.environ, WINEDEBUG="-all"), capture_output=True,
                   text=True)
    return (frames / "run.log").read_text(errors="replace")


def main() -> int:
    compiler = shutil.which("i686-w64-mingw32-gcc")
    missing = [what for what, ok in (
        ("the user-driver Wine (.deps/wine-host-drv/build)", (DRIVER / "build/wine").exists()),
        ("its prefix (.deps/wine-host-drv/prefix)", (DRIVER / "prefix/system.reg").is_file()),
        ("i686-w64-mingw32-gcc", bool(compiler)),
        ("cc", bool(shutil.which("cc")))) if not ok]
    if missing:
        print(f"wine xinput32 skipped: needs {', '.join(missing)}")
        return 0
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        exe = work / "xinput32.exe"
        subprocess.run([compiler, "-O1", "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror", "-o", str(exe),
                        str(SOURCE)], check=True)
        sink = work / "fake_pad_sink.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror", "-o", str(sink), str(SINK),
                        "-lpthread"], check=True)
        prefix = work / "prefix"
        shutil.copytree(DRIVER / "prefix", prefix, symlinks=True)

        log = run(exe, prefix, work / "frames", [str(sink)])
        title, line = xinput_line(log, "title pad")
        want = {"bits": "32", "get0": "0", "caps": "0", "get1": "1167", "buttons_seen": "0xf3ff", "lt_max": "255",
                "rt_max": "255", "set0": "0", "seconds": SECONDS}
        for axis in ("lx", "ly", "rx", "ry"):
            want[f"{axis}_min"] = "-32768"
            want[f"{axis}_max"] = "32767"
        wrong = {name: line[name] for name, value in want.items() if line[name] != value}
        assert not wrong, f"title pad: {wrong} in {log[-2000:]}"
        assert int(line["rumble_events"]) >= 2, f"title pad: rumble_events={line['rumble_events']}"
        assert int(line["packets"]) >= 20, f"title pad: packets={line['packets']}"
        rumbles = re.findall(r"^fake_pad rumble index=0 left=(\d+) right=(\d+)$", log, re.M)
        assert ("65535", "0") in rumbles and ("0", "65535") in rumbles and rumbles[-1] == ("0", "0"), \
            f"title pad: the sink got rumble {rumbles}"

        log = run(exe, prefix, work / "frames-hid", [])
        _, line = xinput_line(log, "no title")
        assert line["get0"] == "1167" and line["get1"] == "1167", f"no title: {line}"
    print(f"wine xinput32 passed: {title}; without the sink get0={line['get0']} get1={line['get1']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
