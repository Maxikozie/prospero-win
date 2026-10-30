#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/orbis_import_stub.py and tools/orbis_bind_table.py on host ELFs: the
stub takes what only the exporter provides plus --also, refuses names the
exporter lacks, data and thread-local symbols; the importer relinked against
the stub imports exactly those, and the bind table lists one slot per such
import with its exporter, and refuses a data import with --no-data."""
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
DATA_IMPORTER = r"""
extern int exported_data;
int read_data(void) { return exported_data; }
"""
SDK = "int sdk_call(int x) { return x; }\n"

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
        sdk = build("libsdk", SDK)
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
        check(listed == ["my_alloc", "wine_only"], f"stub names are my_alloc and wine_only: {listed}")

        for also, word in (("missing_fn", "not defined"), ("exported_data", "OBJECT"), ("exported_tls", "TLS")):
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

        data_importer = build("dataimp", DATA_IMPORTER, exporter)
        proc = run(sys.executable, TABLE, "--elf", data_importer, "--exporter", f"exporter={exporter}",
                   "--out", os.path.join(tmp, "data.bind.txt"), "--no-data", ok=False)
        check(proc.returncode == 1 and "exported_data" in proc.stderr, "a data import from a sibling is refused")
    finally:
        shutil.rmtree(tmp)
    if failures:
        print(f"orbis prx imports: {len(failures)} failure(s)")
        return 1
    print("orbis prx imports passed: stub selection and refusals, NEEDED, slot table, data refusal")
    return 0


if __name__ == "__main__":
    sys.exit(main())
