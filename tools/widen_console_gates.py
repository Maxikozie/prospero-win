#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Widen the PS5 gates of the Wine patch series to the PS4 (Orbis).

The series 0100-0999 selects its console code with ``__PROSPERO__``. The
Orbis build shares most of it (the in-process server, the PRX loader, the
direct-memory and signal-frame patches: the PS4 kernel measured the same
frame), so every ``__PROSPERO__`` test becomes a two-platform test:

    #ifdef __PROSPERO__          -> #if defined(__PROSPERO__) || defined(__ORBIS__)
    #ifndef __PROSPERO__         -> #if !defined(__PROSPERO__) && !defined(__ORBIS__)
    defined(__PROSPERO__)        -> (defined(__PROSPERO__) || defined(__ORBIS__))
    (inside any #if / #elif expression; a leading ! stays correct)

Every line of a patch is rewritten (added, context and removed lines), so
that a later patch's context still matches an earlier patch's output. Patches
named in EXCLUDED stay PS5-only: only their context lines are rewritten (they
quote widened patches), their added lines keep the bare gate and their removed
lines quote each other. ``git apply`` of the whole series on the pinned tree
is the test.

Orbis-specific code goes in patches 1000-1499 under ``#ifdef __ORBIS__``; this
tool never touches those.

Usage: widen_console_gates.py [--check] [--patches DIR]
  --check   exit 1 if any 0100-0999 patch outside EXCLUDED still has a bare
            ``__PROSPERO__`` gate (what the test suite runs)
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# PS5-only patches: the native WoW64 signal entry (raw sigaction through
# libkernel's syscall stub, PS5 FW 12.02 facts) and the FS-base keep that
# builds on its FSBASE_sig. Milestone f revisits both for the PS4.
EXCLUDED = ("0883-", "0903-")

IFDEF = re.compile(r"^([+\- ]\s*)#\s*ifdef\s+__PROSPERO__(\s*(?:/\*.*)?)$")
IFNDEF = re.compile(r"^([+\- ]\s*)#\s*ifndef\s+__PROSPERO__(\s*(?:/\*.*)?)$")
DEFINED = re.compile(r"(?<![\w(])defined\(__PROSPERO__\)")
WIDE = "defined(__PROSPERO__) || defined(__ORBIS__)"


def widen_line(line: str) -> str:
    """Rewrite one patch line without its line ending."""
    m = IFDEF.match(line)
    if m:
        return f"{m.group(1)}#if {WIDE}{m.group(2)}"
    m = IFNDEF.match(line)
    if m:
        return f"{m.group(1)}#if !defined(__PROSPERO__) && !defined(__ORBIS__){m.group(2)}"
    if line[:1] in "+- " and DEFINED.search(line) and WIDE not in line:
        stripped = line[1:].lstrip()
        if stripped.startswith("#"):
            return DEFINED.sub(f"({WIDE})", line)
    return line


def widen(text: str, context_only: bool = False) -> str:
    out = []
    for line in text.splitlines(keepends=True):
        body = line.rstrip("\r\n")
        if context_only and body[:1] != " ":
            out.append(line)
            continue
        out.append(widen_line(body) + line[len(body):])
    return "".join(out)


def bare_gates(text: str) -> list[str]:
    out = []
    for line in text.splitlines():
        if line[:1] not in "+- ":
            continue
        body = line[1:].lstrip()
        if not body.startswith("#"):
            continue
        if "__PROSPERO__" in body and "__ORBIS__" not in body:
            out.append(line)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--patches", default=str(ROOT / "wine/patches"))
    args = ap.parse_args()
    patches = sorted(Path(args.patches).glob("[0-9][0-9][0-9][0-9]-*.patch"))
    bad = 0
    changed = 0
    for patch in patches:
        if int(patch.name[:4]) >= 1000:
            continue
        excluded = patch.name.startswith(EXCLUDED)
        text = patch.read_text()
        if args.check:
            for line in bare_gates(text):
                if excluded and line[:1] != " ":
                    continue
                print(f"{patch.name}: {line}")
                bad += 1
            continue
        new = widen(text, context_only=excluded)
        if new != text:
            patch.write_text(new)
            changed += 1
    if args.check:
        print(f"widen_console_gates: {bad} bare __PROSPERO__ gate(s)")
        return 1 if bad else 0
    print(f"widen_console_gates: {changed} patch(es) rewritten")
    return 0


if __name__ == "__main__":
    sys.exit(main())
