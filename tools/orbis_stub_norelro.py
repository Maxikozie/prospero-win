#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Prepare a stub eboot ELF linked with tools/pw_stub_orbis.ld (-z norelro, --no-rosegment) for
OpenOrbis create-fself so that the eboot gets no PT_SCE_RELRO, and check where its segments land
(ps4-wine milestone i: the PS4's module loader re-protects module 0's PT_SCE_RELRO on every module
load, even after the eboot was released, over the program Wine maps there; and libkernel reads
module 0's data after the release, so the data segment is kept, above Wine's 32-bit areas).

create-fself (pkg/oelf/OELFGenProgramHeaders.go) dereferences the PT_GNU_RELRO header with flags R
unconditionally (line 56: a nil pointer without one), and drops it, emitting no PT_SCE_RELRO, when
the ELF has no .data.rel.ro section (line 87). Its other rules for this layout: a read-only
PT_LOAD is dropped (the text PT_LOAD must hold .rodata itself), and every read-write PT_LOAD is
rewritten to span from the first .data* section to the end of .bss (so there must be exactly one,
starting at .data.sce_process_param, and every writable section must lie inside it).

usage:
  orbis_stub_norelro.py [--layout CODE_END DATA_LOW DATA_HIGH] IN OUT
      Checks those conditions on the linked ELF, then turns its PT_GNU_STACK header (which
      create-fself drops anyway) into an empty PT_GNU_RELRO with flags R at offset 0, which matches
      no PT_LOAD, and writes the result to OUT (IN is left as it is).
  orbis_stub_norelro.py --check-oelf [--layout CODE_END DATA_LOW DATA_HIGH] OELF
      Prints create-fself's program headers (addresses as loaded, from 0x400000) and checks that
      there is no PT_SCE_RELRO.

--layout (addresses as loaded, hex): the read-execute PT_LOAD must lie in [0x400000, CODE_END) and
the read-write one in [DATA_LOW, DATA_HIGH); no PT_LOAD may touch [CODE_END, DATA_LOW). The title's
stub uses 0x404000 0x7fff0000 0x80000000: one code page, and data inside Windows' 64 KiB no-access
region at the top of a 32-bit program's 2 GiB (from 0x80000000 up, large-address-aware programs
allocate). Every failed condition is named and the exit status is nonzero.
"""
import struct
import sys

PT_LOAD, PT_GNU_STACK, PT_GNU_RELRO, PT_SCE_RELRO = 1, 0x6474E551, 0x6474E552, 0x61000010
PF_X, PF_W, PF_R = 1, 2, 4
EBOOT_BASE = 0x400000   # where the PS4 loads an eboot
NAMES = {1: "LOAD", 2: "DYNAMIC", 3: "INTERP", 7: "TLS", 0x6474E550: "GNU_EH_FRAME", PT_GNU_STACK: "GNU_STACK",
         PT_GNU_RELRO: "GNU_RELRO", 0x61000000: "SCE_DYNLIBDATA", 0x61000001: "SCE_PROCPARAM",
         0x61000002: "SCE_MODULE_PARAM", PT_SCE_RELRO: "SCE_RELRO"}


def fail(message):
    sys.stderr.write(f"orbis_stub_norelro: {message}\n")
    sys.exit(1)


def read_elf(path):
    data = bytearray(open(path, "rb").read())
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        fail(f"{path}: not a 64-bit little-endian ELF")
    phoff, shoff = struct.unpack_from("<QQ", data, 0x20)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", data, 0x36)
    headers = []
    for i in range(phnum):
        at = phoff + i * phentsize
        kind, flags, offset, vaddr, paddr, filesz, memsz, align = struct.unpack_from("<IIQQQQQQ", data, at)
        headers.append((at, kind, flags, offset, vaddr, filesz, memsz))
    sections = {}
    if shoff and shnum:
        strings = struct.unpack_from("<Q", data, shoff + shstrndx * shentsize + 0x18)[0]
        for i in range(shnum):
            at = shoff + i * shentsize
            name, kind, flags, addr, offset, size = struct.unpack_from("<IIQQQQ", data, at)
            end = data.index(0, strings + name)
            sections[data[strings + name:end].decode()] = (kind, addr, offset, size, flags)
    return data, headers, sections


def check_layout(headers, layout, what):
    """The loaded PT_LOADs against --layout; returns a report of the read-write one."""
    code_end, data_low, data_high = layout
    report = ""
    for _, kind, flags, _, vaddr, filesz, memsz in headers:
        if kind != PT_LOAD:
            continue
        low, high = EBOOT_BASE + vaddr, EBOOT_BASE + vaddr + memsz
        if flags & PF_W:
            if not (data_low <= low and high <= data_high):
                fail(f"{what}: the read-write PT_LOAD {low:#x}-{high:#x} ({memsz:#x} bytes) is outside "
                     f"[{data_low:#x}, {data_high:#x})")
            report = f"data {low:#x}-{high:#x} ({memsz:#x} of {data_high - data_low:#x} bytes)"
        elif not (EBOOT_BASE <= low and high <= code_end):
            fail(f"{what}: the PT_LOAD {low:#x}-{high:#x} (flags {flags:#x}) is outside [{EBOOT_BASE:#x}, {code_end:#x})")
        if low < data_low and high > code_end:
            fail(f"{what}: the PT_LOAD {low:#x}-{high:#x} spans the gap [{code_end:#x}, {data_low:#x})")
    if not report:
        fail(f"{what}: no read-write PT_LOAD")
    return report


def check_oelf(path, layout):
    _, headers, _ = read_elf(path)
    print(f"{path}: program headers after create-fself (addresses as loaded, from {EBOOT_BASE:#x}):")
    print(f"  {'type':<16} {'vaddr':>12} {'filesz':>10} {'memsz':>10} flags")
    for _, kind, flags, _, vaddr, filesz, memsz in headers:
        name = NAMES.get(kind, f"{kind:#x}")
        rwx = ("R" if flags & PF_R else "-") + ("W" if flags & PF_W else "-") + ("X" if flags & PF_X else "-")
        shown = f"{EBOOT_BASE + vaddr:#x}" if kind in (PT_LOAD, 7, 0x61000001, 0x6474E550, PT_SCE_RELRO) else f"{vaddr:#x}"
        print(f"  {name:<16} {shown:>12} {filesz:>#10x} {memsz:>#10x} {rwx}")
    if any(h[1] == PT_SCE_RELRO for h in headers):
        fail(f"{path}: create-fself emitted a PT_SCE_RELRO: the module loader would re-protect it")
    if layout:
        print(f"orbis_stub_norelro: {path}: {check_layout(headers, layout, path)}; no PT_SCE_RELRO; "
              f"no PT_LOAD in [{layout[0]:#x}, {layout[1]:#x})")
    return 0


def prepare(src, dst, layout):
    data, headers, sections = read_elf(src)
    if ".data.rel.ro" in sections:
        fail("the ELF has a .data.rel.ro section: create-fself would emit a PT_SCE_RELRO (link with pw_stub_orbis.ld)")
    if any(h[1] == PT_GNU_RELRO for h in headers):
        fail("the ELF has a PT_GNU_RELRO: link with -z norelro")
    loads = [h for h in headers if h[1] == PT_LOAD]
    text = [h for h in loads if h[2] == PF_R | PF_X]
    writable = [h for h in loads if h[2] & PF_W]
    if len(text) != 1 or len(writable) != 1 or len(loads) != 2:
        fail("want exactly one read-execute and one read-write PT_LOAD, found " +
             ", ".join(f"{h[2]:#x}@{h[4]:#x}" for h in loads))
    _, _, _, t_offset, t_vaddr, t_filesz, _ = text[0]
    for name in (".rodata", ".eh_frame", ".eh_frame_hdr"):
        if name in sections:
            _, addr, _, size, _ = sections[name]
            if not (t_vaddr <= addr and addr + size <= t_vaddr + t_filesz):
                fail(f"{name} is outside the read-execute PT_LOAD (link with --no-rosegment)")
    param = sections.get(".data.sce_process_param")
    _, _, w_offset, w_vaddr, w_filesz, w_memsz = writable[0][1:]
    if not param or param[2] != w_offset or param[1] != w_vaddr:
        fail("the read-write PT_LOAD does not start at .data.sce_process_param")
    for name, (kind, addr, offset, size, flags) in sections.items():
        if flags & 2 and flags & 1 and not flags & 0x400:   # SHF_ALLOC | SHF_WRITE, not TLS
            if not (w_vaddr <= addr and addr + size <= w_vaddr + w_memsz):
                fail(f"writable section {name} lies outside the read-write PT_LOAD")
    report = check_layout(headers, layout, src) if layout else \
        f"text {t_vaddr:#x}+{t_filesz:#x}, data {w_vaddr:#x}+{w_memsz:#x}"
    stack = [h for h in headers if h[1] == PT_GNU_STACK]
    if not stack:
        fail("no PT_GNU_STACK header to turn into the empty PT_GNU_RELRO create-fself needs")
    struct.pack_into("<IIQQQQQQ", data, stack[0][0], PT_GNU_RELRO, PF_R, 0, 0, 0, 0, 0, 1)
    open(dst, "wb").write(data)
    print(f"orbis_stub_norelro: {dst}: {report}; empty PT_GNU_RELRO for create-fself")
    return 0


def main(argv):
    args = argv[1:]
    if not args or args[0] in ("-h", "--help"):
        sys.stdout.write(__doc__)
        return 0 if args else 2
    oelf = args[0] == "--check-oelf"
    if oelf:
        args = args[1:]
    layout = None
    if args and args[0] == "--layout":
        if len(args) < 4:
            fail("--layout takes CODE_END DATA_LOW DATA_HIGH")
        try:
            layout = tuple(int(value, 16) for value in args[1:4])
        except ValueError:
            fail("--layout takes three hex addresses")
        if not (EBOOT_BASE < layout[0] <= layout[1] < layout[2]):
            fail("--layout wants 0x400000 < CODE_END <= DATA_LOW < DATA_HIGH")
        args = args[4:]
    if oelf:
        if len(args) != 1:
            fail("--check-oelf takes one file")
        return check_oelf(args[0], layout)
    if len(args) != 2:
        fail("want IN OUT (see --help)")
    return prepare(args[0], args[1], layout)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
