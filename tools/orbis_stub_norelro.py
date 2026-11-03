#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Prepare a stub eboot ELF linked with tools/pw_stub_orbis.ld (-z norelro, --no-rosegment) for
OpenOrbis create-fself so that the eboot gets no PT_SCE_RELRO (ps4-wine milestone i: the PS4's
module loader re-protects module 0's PT_SCE_RELRO on every module load, even after the eboot was
released, over the program Wine maps there).

create-fself (pkg/oelf/OELFGenProgramHeaders.go) dereferences the PT_GNU_RELRO header with flags R
unconditionally (line 56: a nil pointer without one), and drops it, emitting no PT_SCE_RELRO, when
the ELF has no .data.rel.ro section (line 87). Its other rules for this layout: a read-only
PT_LOAD is dropped (the text PT_LOAD must hold .rodata itself), and every read-write PT_LOAD is
rewritten to span from the first .data* section to the end of .bss (so there must be exactly one,
starting at .data.sce_process_param, and every writable section must lie inside it).

This tool checks those conditions on the linked ELF, then turns its PT_GNU_STACK header (which
create-fself drops anyway) into an empty PT_GNU_RELRO with flags R at offset 0, which matches no
PT_LOAD. It writes the result to OUT (IN is left as it is) and exits nonzero, naming the failed
condition, if the ELF does not have the layout.

usage: orbis_stub_norelro.py IN OUT
"""
import struct
import sys

PT_LOAD, PT_GNU_STACK, PT_GNU_RELRO = 1, 0x6474E551, 0x6474E552
PF_X, PF_W, PF_R = 1, 2, 4


def fail(message):
    sys.stderr.write(f"orbis_stub_norelro: {message}\n")
    sys.exit(1)


def main(argv):
    if len(argv) != 3 or argv[1] in ("-h", "--help"):
        sys.stdout.write(__doc__)
        return 0 if len(argv) == 2 else 2
    data = bytearray(open(argv[1], "rb").read())
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        fail(f"{argv[1]}: not a 64-bit little-endian ELF")
    phoff, shoff = struct.unpack_from("<QQ", data, 0x20)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", data, 0x36)
    strings = struct.unpack_from("<Q", data, shoff + shstrndx * shentsize + 0x18)[0]

    sections = {}
    for i in range(shnum):
        at = shoff + i * shentsize
        name, kind, flags, addr, offset, size = struct.unpack_from("<IIQQQQ", data, at)
        end = data.index(0, strings + name)
        sections[data[strings + name:end].decode()] = (kind, addr, offset, size, flags)
    headers = []
    for i in range(phnum):
        at = phoff + i * phentsize
        kind, flags, offset, vaddr, paddr, filesz, memsz, align = struct.unpack_from("<IIQQQQQQ", data, at)
        headers.append((at, kind, flags, offset, vaddr, filesz, memsz))

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
    stack = [h for h in headers if h[1] == PT_GNU_STACK]
    if not stack:
        fail("no PT_GNU_STACK header to turn into the empty PT_GNU_RELRO create-fself needs")
    struct.pack_into("<IIQQQQQQ", data, stack[0][0], PT_GNU_RELRO, PF_R, 0, 0, 0, 0, 0, 1)
    open(argv[2], "wb").write(data)
    print(f"orbis_stub_norelro: {argv[2]}: text {t_vaddr:#x}+{t_filesz:#x}, data {w_vaddr:#x}+{w_memsz:#x}, "
          "empty PT_GNU_RELRO for create-fself")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
