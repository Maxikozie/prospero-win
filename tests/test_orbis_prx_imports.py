#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/orbis_import_stub.py and tools/orbis_bind_table.py on host ELFs: the
stub takes what only the exporter provides plus --also, refuses names the
exporter lacks, data and thread-local symbols; the importer relinked against
the stub imports exactly those, and the bind table lists one slot per such
import with its exporter, and refuses a data import with --no-data. With two
exporters (winevulkan from ntdll and win32u) a name goes to the first that
defines it, --also names only to the first, one stub each, NEEDED in that
order, and the table names each slot's exporter."""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STUB = os.path.join(ROOT, "tools", "orbis_import_stub.py")
TABLE = os.path.join(ROOT, "tools", "orbis_bind_table.py")

EXPORTER = r"""
int wine_only(int x) { return x + 1; }
int wine_only_too(int x) { return x + 2; }
int shared_helper(int x) { return x * 2; }
void *my_alloc(unsigned long n) { (void)n; return 0; }
int exported_data = 7;
__thread int exported_tls = 3;
"""
# The importer calls wine_only (only the exporter has it), sdk_call (an SDK
# stub library has it) and shared_helper (its own static copy below).
IMPORTER = r"""
extern int wine_only(int);
extern int sdk_call(int);
extern void *my_alloc(unsigned long);
int shared_helper(int x) { return x * 3; }
int entry(int x) { return wine_only(x) + sdk_call(x) + shared_helper(x) + (my_alloc(1) != 0); }
"""
SECOND = r"""
int second_only(int x) { return x + 4; }
int wine_only(int x) { return x + 5; }
void *second_alloc(unsigned long n) { (void)n; return 0; }
"""
TWO_IMPORTER = r"""
extern int wine_only(int);
extern int second_only(int);
extern void *my_alloc(unsigned long);
int entry2(int x) { return wine_only(x) + second_only(x) + (my_alloc(1) != 0); }
"""
DATA_IMPORTER = r"""
extern int exported_data;
int read_data(void) { return exported_data; }
"""
SDK = "int sdk_call(int x) { return x; }\nint wine_only_too(int x) { return x; }\n"

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


def which(*names):
    for name in names:
        if shutil.which(name):
            return name
    return None


def run(*args, ok=True):
    proc = subprocess.run(list(args), capture_output=True, text=True)
    if ok and proc.returncode:
        raise SystemExit(f"{' '.join(args)}: {proc.stderr}")
    return proc


def main():
    cc = which("clang-18", "clang")
    ld = which("ld.lld-18", "ld.lld")
    if not (cc and ld and which("llvm-readelf") and which("llvm-nm")):
        print("skip: clang, ld.lld, llvm-readelf and llvm-nm 18 are required (PATH=/usr/lib/llvm-18/bin:$PATH)")
        return 0
    tmp = tempfile.mkdtemp(prefix="orbis-imports-")
    try:
        def build(name, source, *extra):
            path = os.path.join(tmp, name)
            with open(path + ".c", "w") as f:
                f.write(source)
            run(cc, "-fPIC", "-O1", "-c", path + ".c", "-o", path + ".o")
            run(ld, "-shared", "-o", path + ".so", path + ".o", *extra)
            return path + ".so"

        toolchain = os.path.join(tmp, "oo")
        os.makedirs(os.path.join(toolchain, "lib"))
        sdk = build("libsdk", SDK, "-soname", "libsdk.so")
        shutil.copy(sdk, os.path.join(toolchain, "lib", "libsdk.so"))
        exporter = build("exporter", EXPORTER, "-soname", "exporter.so")
        # First link: the importer alone (undefined wine_only, sdk_call, my_alloc).
        first = build("first", IMPORTER, sdk)
        stub_s = os.path.join(tmp, "stub.s")
        names = os.path.join(tmp, "names.txt")
        proc = run(sys.executable, STUB, "--first-link", first, "--exporter", exporter, "--toolchain", toolchain,
                   "--also", "my_alloc", "--out", stub_s, "--list", names, ok=False)
        check(proc.returncode == 0, f"stub generated ({proc.stderr.strip()})")
        listed = open(names).read().split() if os.path.exists(names) else []
        # A stub library the importer does not need may export the same name: no conflict.
        unneeded = build("libother", "int my_alloc_other(int x) { return x; }\nvoid *my_alloc(unsigned long n) { (void)n; return 0; }\n")
        shutil.copy(unneeded, os.path.join(toolchain, "lib", "libother.so"))
        proc = run(sys.executable, STUB, "--first-link", first, "--exporter", exporter, "--toolchain", toolchain,
                   "--also", "my_alloc", "--out", stub_s, ok=False)
        check(proc.returncode == 0, f"a name an unneeded stub library exports is taken ({proc.stderr.strip()})")
        check(listed == ["my_alloc", "wine_only"], f"stub names are my_alloc and wine_only: {listed}")

        for also, word in (("missing_fn", "not defined"), ("exported_data", "OBJECT"), ("exported_tls", "TLS"),
                           ("wine_only_too", "SDK stub library the importer needs exports it too")):
            proc = run(sys.executable, STUB, "--first-link", first, "--exporter", exporter, "--toolchain", toolchain,
                       "--also", also, "--out", os.path.join(tmp, "bad.s"), ok=False)
            check(proc.returncode == 1 and word in proc.stderr, f"--also {also} refused ({word})")

        # The stub, linked under the exporter's soname, and the importer against it.
        run(cc, "-c", stub_s, "-o", os.path.join(tmp, "stub.o"))
        stub_so = os.path.join(tmp, "exporter.stub.so")
        run(ld, "-shared", "-soname", "exporter.so", "-o", stub_so, os.path.join(tmp, "stub.o"))
        importer = build("importer", IMPORTER, stub_so, sdk)
        needed = run("llvm-readelf", "-d", importer).stdout
        check("[exporter.so]" in needed, "importer records exporter.so as NEEDED")
        table = os.path.join(tmp, "importer.bind.txt")
        proc = run(sys.executable, TABLE, "--elf", importer, "--exporter", f"exporter={stub_so}", "--out", table,
                   "--no-data", ok=False)
        check(proc.returncode == 0, "bind table written")
        lines = open(table).read().splitlines()
        slots = sorted(line.split()[4] for line in lines if line.startswith("slot "))
        check(lines[0] == "module importer" and lines[1].startswith("first_vaddr 0x"), f"table header: {lines[:2]}")
        check(slots == ["my_alloc", "wine_only"], f"one slot per sibling import: {slots}")
        check(all(line.split()[3] == "exporter" for line in lines if line.startswith("slot ")), "slots name the exporter")

        # Two exporters in priority order.
        second = build("second", SECOND, "-soname", "second.so")
        first2 = build("first2", TWO_IMPORTER)
        two_s = os.path.join(tmp, "two_{exporter}.s")
        names2 = os.path.join(tmp, "names2.txt")
        proc = run(sys.executable, STUB, "--first-link", first2, "--exporter", f"exporter={exporter}",
                   "--exporter", f"second={second}", "--toolchain", toolchain, "--also", "my_alloc",
                   "--out", two_s, "--list", names2, ok=False)
        check(proc.returncode == 0, f"two-exporter stubs generated ({proc.stderr.strip()})")
        stub_names = {}
        for exp in ("exporter", "second"):
            path = two_s.replace("{exporter}", exp)
            stub_names[exp] = [l.split()[1] for l in open(path).read().splitlines() if l.startswith("\t.globl")] \
                if os.path.exists(path) else None
        check(stub_names == {"exporter": ["my_alloc", "wine_only"], "second": ["second_only"]},
              f"a name in both goes to the first exporter: {stub_names}")
        proc = run(sys.executable, STUB, "--first-link", first2, "--exporter", f"exporter={exporter}",
                   "--exporter", f"second={second}", "--toolchain", toolchain, "--also", "second_alloc",
                   "--out", two_s, ok=False)
        check(proc.returncode == 1 and "second_alloc: not defined" in proc.stderr,
              "--also names come from the first exporter only")
        proc = run(sys.executable, STUB, "--first-link", first2, "--exporter", f"exporter={exporter}",
                   "--exporter", f"second={second}", "--toolchain", toolchain, "--out", stub_s, ok=False)
        check(proc.returncode == 1 and "{exporter}" in proc.stderr, "several exporters need {exporter} in --out")
        so = {}
        for exp in ("exporter", "second"):
            obj = os.path.join(tmp, f"two_{exp}.o")
            run(cc, "-c", two_s.replace("{exporter}", exp), "-o", obj)
            so[exp] = os.path.join(tmp, f"two_{exp}.stub.so")
            run(ld, "-shared", "-soname", f"{exp}.so", "-o", so[exp], obj)
        importer2 = build("importer2", TWO_IMPORTER, so["exporter"], so["second"])
        needed2 = [l.split("[", 1)[1].rstrip("]") for l in run("llvm-readelf", "-d", importer2).stdout.splitlines()
                   if "(NEEDED)" in l]
        check(needed2[:2] == ["exporter.so", "second.so"], f"NEEDED in priority order: {needed2}")
        table2 = os.path.join(tmp, "importer2.bind.txt")
        proc = run(sys.executable, TABLE, "--elf", importer2, "--exporter", f"exporter={so['exporter']}",
                   "--exporter", f"second={so['second']}", "--out", table2, "--no-data", ok=False)
        slots2 = sorted((l.split()[4], l.split()[3]) for l in open(table2).read().splitlines() if l.startswith("slot "))
        check(proc.returncode == 0 and slots2 == [("my_alloc", "exporter"), ("second_only", "second"),
                                                  ("wine_only", "exporter")],
              f"each slot names its exporter: {slots2}")

        data_importer = build("dataimp", DATA_IMPORTER, exporter)
        proc = run(sys.executable, TABLE, "--elf", data_importer, "--exporter", f"exporter={exporter}",
                   "--out", os.path.join(tmp, "data.bind.txt"), "--no-data", ok=False)
        check(proc.returncode == 1 and "exported_data" in proc.stderr, "a data import from a sibling is refused")
    finally:
        shutil.rmtree(tmp)
    if failures:
        print(f"orbis prx imports: {len(failures)} failure(s)")
        return 1
    print("orbis prx imports passed: stub selection and refusals, NEEDED, slot table, data refusal, two exporters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
