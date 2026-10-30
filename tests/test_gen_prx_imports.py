#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/gen_prx_imports.py: the generated thunks, compiled on the host with
a resolver over local functions, reach those functions once bound, count a
name the resolver lacks, and the generator refuses bad or repeated names."""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "gen_prx_imports.py")

MAIN = r"""
#include <stdio.h>
#include <string.h>
extern int pw_bind_add(int, int);
extern const char *pw_bind_name(void);
extern int pw_prx_bind_imports(const void *(*)(void *, const char *), void *);
static int real_add(int a, int b) { return a + b; }
static const char *real_name(void) { return "pw_bind_lib"; }
static int lookups;
static const void *resolve(void *context, const char *name)
{
    lookups++;
    if (context != (void *)&lookups) return NULL;
    if (!strcmp(name, "pw_bind_add")) return (const void *)real_add;
    if (!strcmp(name, "pw_bind_name")) return (const void *)real_name;
    return NULL;
}
int main(void)
{
    int missing = pw_prx_bind_imports(resolve, &lookups);
    printf("missing=%d lookups=%d add=%d name=%s\n", missing, lookups, pw_bind_add(40, 2), pw_bind_name());
    return 0;
}
"""


def main():
    cc = shutil.which("clang-18") or shutil.which("clang") or shutil.which("cc")
    if not cc:
        print("skip: no C compiler")
        return 0
    failures = []
    tmp = tempfile.mkdtemp(prefix="gen-prx-imports-")
    try:
        gen = os.path.join(tmp, "imports.c")
        rc = subprocess.run([sys.executable, TOOL, gen, "pw_bind_add", "pw_bind_name", "pw_bind_absent"]).returncode
        if rc:
            failures.append("generator failed")
        else:
            main_c = os.path.join(tmp, "main.c")
            with open(main_c, "w") as f:
                f.write(MAIN)
            exe = os.path.join(tmp, "t")
            build = subprocess.run([cc, "-O2", "-Wall", "-Wextra", "-Werror", "-fPIE", "-pie", gen, main_c, "-o", exe],
                                   capture_output=True, text=True)
            if build.returncode:
                failures.append(f"generated thunks do not build: {build.stderr}")
            else:
                out = subprocess.run([exe], capture_output=True, text=True).stdout.strip()
                if out != "missing=1 lookups=3 add=42 name=pw_bind_lib":
                    failures.append(f"thunks: {out}")
        for bad in (["9x"], ["a", "a"], ["a-b"]):
            if subprocess.run([sys.executable, TOOL, os.path.join(tmp, "bad.c")] + bad,
                              capture_output=True).returncode != 1:
                failures.append(f"names {bad} accepted")
    finally:
        shutil.rmtree(tmp)
    for failure in failures:
        print(f"FAIL {failure}")
    if failures:
        return 1
    print("gen prx imports passed: bound thunks call through, a missing name is counted, bad names refused")
    return 0


if __name__ == "__main__":
    sys.exit(main())
