/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The layout of a stub eboot that is released from 0x400000 before Wine starts (ps4-wine
 * milestone i): two segments, read-execute then read-write, and no read-only one.
 *
 * Why: the PS4's module loader re-protects module 0's PT_SCE_RELRO segment on every module load,
 * read-write during the load and read-only at its end, even after the eboot was unmapped, over
 * whatever is mapped there by then (ps4-wine LOG L23-L25: a 32-bit program's .text and .data at
 * 0x404000-0x410000 lost execute and write on each PRX load). create-fself turns the ELF's
 * PT_GNU_RELRO into that segment, covering everything from .data.rel.ro up to the first .data*
 * section. Here every writable section follows .data.sce_process_param, .data.rel.ro is folded
 * into .data, and tools/orbis_stub_norelro.py leaves create-fself an empty PT_GNU_RELRO it drops
 * (link with -z norelro and --no-rosegment, then run that tool on the ELF).
 *
 * Read-execute: the program interpreter's path first (the loader reads it at the start of the
 * text), then code, read-only data and the unwind tables, in one PT_LOAD (--no-rosegment).
 * Read-write, from a 16 KiB boundary: the process parameters first (create-fself starts the data
 * segment at the first .data* section), then everything else that is written. */
SECTIONS
{
    .text : ALIGN(0x4000)
    {
        __text_start = .;
        /* "/libexec/ld-elf.so.1", NUL-padded to 32 bytes */
        BYTE(0x2f) BYTE(0x6c) BYTE(0x69) BYTE(0x62) BYTE(0x65) BYTE(0x78) BYTE(0x65) BYTE(0x63)
        BYTE(0x2f) BYTE(0x6c) BYTE(0x64) BYTE(0x2d) BYTE(0x65) BYTE(0x6c) BYTE(0x66) BYTE(0x2e)
        BYTE(0x73) BYTE(0x6f) BYTE(0x2e) BYTE(0x31) BYTE(0x00) BYTE(0x00) BYTE(0x00) BYTE(0x00)
        LONG(0) LONG(0)
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .eh_frame : { __eh_frame_start = .; KEEP(*(.eh_frame)) __eh_frame_end = .; }
    .eh_frame_hdr : { __eh_frame_hdr_start = .; KEEP(*(.eh_frame_hdr)) __eh_frame_hdr_end = .; }

    .data.sce_process_param : ALIGN(0x4000) { KEEP(*(.data.sce_process_param)) }
    .data.sce_module_param : { KEEP(*(.data.sce_module_param)) }
    .dynamic : { *(.dynamic) }
    .got : { *(.got) }
    /* The PS4's loader wants a .got.plt; naming the table's symbol makes the linker emit one. */
    .got.plt : { *(.got.plt) __pw_stub_got_plt = _GLOBAL_OFFSET_TABLE_; }
    .init_array : { KEEP(*(.init_array .init_array.*)) }
    .fini_array : { KEEP(*(.fini_array .fini_array.*)) }
    .data : { *(.data.rel.ro .data.rel.ro.*) *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
}
