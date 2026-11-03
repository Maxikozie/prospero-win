#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The PS4 title: native/orbis/wine64_main_orbis.c with the start sequence,
# diagnostics and PRX descriptor code it shares with the PS5 title, compiled
# by tools/orbis-clang and linked as an app PRX, ps4wine.prx (the Wine PRXs'
# recipe: -e module_start, a PRXDESC1 descriptor exporting pw_title_main,
# orbis-compat's corrections ahead of the SDK's libc), and a stub eboot,
# native/orbis/pw_stub_orbis.c (the SDK's link.x and crt1.o), that loads it
# from /app0/sce_module/ps4wine.prx. The title releases the eboot's segments
# before Wine starts, so that a PE fixed at 0x400000 can map there (ps4-wine
# milestone i). The packaging (create-fself --eboot, ps4wine.prx into
# sce_module/, param.sfo, the .pkg) is the consumer's: this prints the
# eboot's path last; ps4wine.prx is next to it.
#
# Checks: no raw syscall; no sigsetjmp/siglongjmp import; ps4wine.prx exports
# module_start and pw_title_main and has no PT_LOAD mid-page; the stub NEEDs
# every Sony library ps4wine.prx NEEDs; the stub's PT_TLS (the storage for
# every PRX's local-exec thread-locals) is .tbss only and at least as large
# as the PT_TLS of ps4wine.prx and of each Wine PRX in $WINE_PRX_DIR
# (default .deps/wine-orbis/prx; none skips the Wine PRXs), each rounded to its
# alignment; the stub also NEEDs every Sony library those Wine PRXs NEED.
#
# Also builds pw_probe_mod.prx, a module linked like the Wine PRXs, which
# the title loads to check the recipe on the console, and milestone d1's
# binding modules: pw_bind_lib.prx (the exporter), pw_bind_use.prx and
# pw_bind_early.prx (loader imports of its functions, with their
# .bind.txt slot tables), pw_bind_data.prx (a loader import of its data
# object) and pw_bind_thunk.prx (generated thunks, no loader imports).
#
# Usage:
#   tools/build_native_orbis.sh [--out DIR] [--netlog-host IP] [--netlog-port N]
#       [--build-id ID] [--data-root DIR]   (the title's root on /data, default /data/prospero-win)
# Environment: OO_PS4_TOOLCHAIN, ORBIS_COMPAT (include/ and build/), LLVM_BIN,
# PW_VULKAN_INCLUDE (the directory holding vulkan/vulkan.h: the milestone e
# stages drive libvulkan.prx; mesa-ps4's include/).
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$root/.deps/native-orbis
netlog_host=
netlog_port=18194
build_id=
data_root=/data/prospero-win

fail() { echo "build_native_orbis: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case $1 in
    --out) out=$2; shift ;;
    --netlog-host) netlog_host=$2; shift ;;
    --netlog-port) netlog_port=$2; shift ;;
    --build-id) build_id=$2; shift ;;
    --data-root) data_root=$2; shift ;;
    -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) fail "unknown argument $1" ;;
    esac
    shift
done
: "${OO_PS4_TOOLCHAIN:?set OO_PS4_TOOLCHAIN}"
: "${ORBIS_COMPAT:?set ORBIS_COMPAT (include/ and build/)}"
[ -f "${PW_VULKAN_INCLUDE:-}/vulkan/vulkan.h" ] || fail "set PW_VULKAN_INCLUDE to a directory holding vulkan/vulkan.h"
[ -n "${LLVM_BIN:-}" ] && PATH="$LLVM_BIN:$PATH"
export PATH OO_PS4_TOOLCHAIN
export ORBIS_COMPAT_INCLUDE="$ORBIS_COMPAT/include"
compat_lib=$ORBIS_COMPAT/build
cc=$root/tools/orbis-clang
fself=$OO_PS4_TOOLCHAIN/bin/linux/create-fself
[ -x "$fself" ] || fself=$OO_PS4_TOOLCHAIN/bin/create-fself
for tool in clang ld.lld llvm-objdump llvm-readelf; do
    command -v "$tool" >/dev/null || fail "$tool is required"
done
[ -f "$OO_PS4_TOOLCHAIN/link.x" ] && [ -f "$OO_PS4_TOOLCHAIN/lib/crt1.o" ] || fail "no link.x/crt1.o in $OO_PS4_TOOLCHAIN"
[ -f "$compat_lib/liborbis-compat.a" ] || fail "no liborbis-compat.a under $compat_lib"

if [ -z "$build_id" ]; then
    if build_id=$(git -C "$root" rev-parse --short=7 HEAD 2>/dev/null); then
        git -C "$root" diff --quiet HEAD -- 2>/dev/null || build_id="$build_id+dirty"
    else
        build_id=unknown
    fi
fi

rm -rf "$out"
mkdir -p "$out/obj"
common="-std=gnu11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
-I$root/include -I$root/src -I$root/native -I$root/native/orbis -I$root/wine/ps5 -I$root/wine/orbis -I$PW_VULKAN_INCLUDE \
-DPW_BUILD_ID=\"$build_id\" -DPW_ORBIS_NETLOG_HOST=\"$netlog_host\" -DPW_ORBIS_NETLOG_PORT=$netlog_port \
-DPW_ROOT=\"$data_root\""
sources="native/orbis/wine64_main_orbis.c native/orbis/orbislog/orbislog.c native/pw_diagnostics.c \
src/pw_result.c src/pw_wine_start.c wine/ps5/pw_wine_prx.c native/orbis/pw_stage_orbis.c \
native/orbis/pw_milestone_orbis.c native/orbis/pw_videoout_orbis.c native/orbis/pw_pad_orbis.c \
native/pw_wine_display.c src/pw_game_profile.c src/pw_app_profile.c src/pw_script_input.c \
native/orbis/pw_vk_stage_orbis.c native/orbis/pw_cursor_probe_orbis.c native/orbis/pw_hist.c native/orbis/pw_loaddll_orbis.c src/pw_wine_launch.c \
native/pw_hid_ps5.c src/pw_hid.c src/pw_eboot_release.c native/pw_pad_ps5.c src/pw_pad.c \
native/orbis/pw_pad_ps4_orbis.c"
objects=""
for source in $sources; do
    object=$out/obj/$(basename "${source%.c}").o
    # shellcheck disable=SC2086
    "$cc" $common -c "$root/$source" -o "$object" || fail "cannot compile $source"
    objects="$objects $object"
done
"$cc" $common -c "$root/native/orbis/pw_stub_orbis.c" -o "$out/obj/pw_stub_orbis.o" || fail "cannot compile the stub"
python3 "$root/tools/gen_prx_descriptor.py" --orbis "$out/obj/ps4wine_desc.c" pw_title_main ||
    fail "cannot generate the title's descriptor"
"$cc" -std=c11 -O2 -Wall -Wextra -Werror -I"$root/wine/ps5" -c "$out/obj/ps4wine_desc.c" -o "$out/obj/ps4wine_desc.o" ||
    fail "cannot compile the title's descriptor"
# A raw syscall instruction outside libkernel kills the process.
if llvm-objdump -d "$out"/obj/*.o | grep -qw syscall; then fail "a raw syscall instruction in the title's objects"; fi

# The title PRX (tools/build_wine_orbis.sh's ld_prx recipe, without lazy or unresolved symbols).
sce_libs="-lSceNet -lSceSystemService -lSceUserService -lSceVideoOut -lScePad -lSceAudioOut -lSceSysmodule -lSceGnmDriver"
# shellcheck disable=SC2086
ld.lld -m elf_x86_64 -pie --export-dynamic -e module_start --script "$OO_PS4_TOOLCHAIN/link.x" \
    -T "$root/wine/ps5/prx_eh_frame.ld" --eh-frame-hdr --no-rosegment -z max-page-size=0x4000 -soname ps4wine.so \
    $objects "$out/obj/ps4wine_desc.o" -L"$compat_lib" -lorbis-compat "$compat_lib"/libc16/libc16_*.o \
    -L"$OO_PS4_TOOLCHAIN/lib" -lc -lkernel -lc++ $sce_libs \
    -o "$out/ps4wine.shared.elf" --Map="$out/ps4wine.map" || fail "the title PRX does not link"
# libc.a has setjmp/longjmp/siglongjmp but no sigsetjmp: a sigsetjmp would bind to libkernel's
# (FreeBSD's jmp_buf) and pair with musl's siglongjmp (milestone e1: the stage recovery jumped to 0x20).
if llvm-nm -D "$out/ps4wine.shared.elf" | grep -qE ' U (sigsetjmp|siglongjmp)$'; then
    fail "the title imports sigsetjmp/siglongjmp; use setjmp/longjmp (one libc's pair)"
fi
syms=$(llvm-readelf --dyn-syms "$out/ps4wine.shared.elf")
for name in module_start pw_title_main; do
    printf '%s\n' "$syms" | grep -qE "GLOBAL +DEFAULT +[0-9]+ $name\$" || fail "ps4wine.prx does not export $name"
done
# A PT_LOAD that starts mid-page is ENOEXEC for an app PRX.
for vaddr in $(llvm-readelf -lW "$out/ps4wine.shared.elf" | awk '$1 == "LOAD" { print $3 }'); do
    [ $((vaddr % 16384)) -eq 0 ] || fail "ps4wine.prx has a PT_LOAD at $vaddr, not 16 KiB aligned"
done
(cd "$out" && "$fself" -in=ps4wine.shared.elf -out=ps4wine.oelf --lib=ps4wine.prx -libname=ps4wine \
    --paid 0x3800000000000011 > /dev/null) || fail "create-fself refused ps4wine"
rm -f "$out/ps4wine.oelf"
chmod 755 "$out/ps4wine.prx"

# The stub eboot: NEEDs the same Sony libraries, so the loader has them when it loads the PRX.
# Laid out without a read-only segment (tools/pw_stub_orbis.ld): the PS4's module loader re-protects
# module 0's PT_SCE_RELRO on every module load, even after the release, over the program Wine maps
# there (ps4-wine LOG L23-L26); tools/orbis_stub_norelro.py checks the layout and leaves create-fself
# the empty PT_GNU_RELRO it needs, so the eboot gets none.
# shellcheck disable=SC2086
ld.lld -m elf_x86_64 -pie -z norelro --no-rosegment --script "$root/tools/pw_stub_orbis.ld" --eh-frame-hdr \
    "$out/obj/pw_stub_orbis.o" "$out/obj/pw_wine_prx.o" \
    -L"$OO_PS4_TOOLCHAIN/lib" -lc -lkernel $sce_libs \
    "$OO_PS4_TOOLCHAIN/lib/crt1.o" -o "$out/eboot.linked.elf" --Map="$out/eboot.map" || fail "the stub does not link"
# Where the segments land: code in the one page 0x400000-0x404000, which the release unmaps; data
# in 0x7fff0000-0x80000000, which it keeps: Windows' 64 KiB no-access region at the top of a 32-bit
# program's 2 GiB, above Wine's 32-bit reservations (from 0x80000000 up, large-address-aware
# programs allocate). Checked on the linked ELF and on create-fself's output.
stub_layout="0x404000 0x7fff0000 0x80000000"
# shellcheck disable=SC2086
python3 -I "$root/tools/orbis_stub_norelro.py" --layout $stub_layout "$out/eboot.linked.elf" "$out/eboot.elf" ||
    fail "the stub does not have the layout create-fself needs (tools/orbis_stub_norelro.py)"
(cd "$out" && "$fself" -in=eboot.elf -out=eboot.check.oelf --eboot eboot.check.bin \
    --paid 0x3800000000000011 > /dev/null) || fail "create-fself refused the stub"
# shellcheck disable=SC2086
python3 -I "$root/tools/orbis_stub_norelro.py" --check-oelf --layout $stub_layout "$out/eboot.check.oelf" ||
    fail "create-fself's stub has a PT_SCE_RELRO or a segment outside its place"
rm -f "$out/eboot.check.oelf" "$out/eboot.check.bin"

# The release reads module 0's segments through pw_prx_parse_module_info, which takes at most four.
loads=$(llvm-readelf -lW "$out/eboot.elf" | grep -c '^ *LOAD ')
[ "$loads" -le 4 ] || fail "the stub has $loads PT_LOADs; module info lists at most 4 (PW_PRX_MAX_SEGMENTS)"
needed() { llvm-readelf -d "$1" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sed 's|.*/||' | LC_ALL=C sort -u; }
needed "$out/eboot.elf" > "$out/obj/stub.needed"
# The Wine PRXs the title loads: their PT_TLS and their Sony libraries are checked against the
# stub too (WINE_PRX_DIR=none skips them, for a title built without a Wine tree).
wine_prx_dir=${WINE_PRX_DIR:-$root/.deps/wine-orbis/prx}
if [ "$wine_prx_dir" != none ]; then
    ls "$wine_prx_dir"/*.shared.elf > /dev/null 2>&1 ||
        fail "no Wine PRXs in $wine_prx_dir to check the stub against (build them, or WINE_PRX_DIR=none)"
fi
checked=ps4wine
for elf in "$out/ps4wine.shared.elf" "$wine_prx_dir"/*.shared.elf; do
    [ -f "$elf" ] || continue
    missing=$(needed "$elf" | grep '^libSce' | LC_ALL=C comm -23 - "$out/obj/stub.needed" | tr '\n' ' ')
    [ -z "$missing" ] || fail "the stub does not NEED what $(basename "$elf") NEEDs: $missing"
done
# TLS: "filesz memsz align" in decimal, or "0 0 1" without a PT_TLS. A PRX's local-exec offsets
# reach back memsz rounded up to its alignment from the thread pointer; the stub's block, rounded
# the same way, must cover that.
tls_of() {
    set -- $(llvm-readelf -lW "$1" | awk '$1 == "TLS" { print $5, $6, $NF }')
    [ $# -eq 3 ] && echo "$(($1)) $(($2)) $(($3))" || echo "0 0 1"
}
round_up() { [ "$2" -gt 1 ] && echo $((($1 + $2 - 1) / $2 * $2)) || echo "$1"; }
set -- $(tls_of "$out/eboot.elf"); stub_filesz=$1; stub_memsz=$2; stub_block=$(round_up "$2" "$3")
[ "$stub_memsz" -gt 0 ] || fail "the stub has no PT_TLS"
[ "$stub_filesz" -eq 0 ] || fail "the stub's PT_TLS has $stub_filesz initialised bytes: its image is released with the eboot"
for elf in "$out/ps4wine.shared.elf" "$wine_prx_dir"/*.shared.elf; do
    [ -f "$elf" ] || continue
    set -- $(tls_of "$elf")
    block=$(round_up "$2" "$3")
    [ "$block" -le "$stub_block" ] ||
        fail "$(basename "$elf")'s TLS block is $block bytes, more than the stub's $stub_block"
    [ "$elf" = "$out/ps4wine.shared.elf" ] || checked="$checked $(basename "${elf%.shared.elf}")"
done
tls_checked=$checked
echo "title: stub PT_TLS $stub_memsz bytes .tbss (block $stub_block); TLS and Sony libraries checked against: $tls_checked" >&2

# The probe modules, linked as tools/build_wine_orbis.sh links the Wine PRXs:
# pw_probe_mod (the recipe) and pw_probe_tls (the same with libc's TLS
# variable pulled in, as ntdll has it).
probe_module() {
    python3 "$root/tools/gen_prx_descriptor.py" --orbis "$out/obj/$1_desc.c" pw_probe_mod_started
    # shellcheck disable=SC2086
    "$cc" -std=c11 -O2 -Wall -Wextra -Werror -I"$root/wine/ps5" -c "$out/obj/$1_desc.c" -o "$out/obj/$1_desc.o"
    # shellcheck disable=SC2086
    "$cc" $common $2 -c "$root/native/orbis/pw_probe_mod.c" -o "$out/obj/$1.o"
    ld.lld -m elf_x86_64 -pie --export-dynamic -e module_start --script "$OO_PS4_TOOLCHAIN/link.x" \
        -T "$root/wine/ps5/prx_eh_frame.ld" --eh-frame-hdr --no-rosegment -z max-page-size=0x4000 -soname "$1.prx" -z lazy \
        --warn-unresolved-symbols -o "$out/$1.shared.elf" \
        "$out/obj/$1.o" "$out/obj/$1_desc.o" -L"$OO_PS4_TOOLCHAIN/lib" -lc -lkernel ||
        fail "the probe module $1 does not link"
    llvm-readelf --dyn-syms "$out/$1.shared.elf" | grep -q 'GLOBAL DEFAULT .* module_start$' ||
        fail "$1: module_start is not a default-visibility export"
    (cd "$out" && "$fself" -in="$1.shared.elf" -out="$1.oelf" --lib="$1.prx" \
        --paid 0x3800000000000011 > /dev/null) || fail "create-fself refused $1"
}
probe_module pw_probe_mod ""
probe_module pw_probe_tls "-DPW_PROBE_TLS"
llvm-readelf -l "$out/pw_probe_tls.shared.elf" | grep -q '^  TLS' || fail "pw_probe_tls carries no TLS segment"

# Milestone d1's binding modules, with the names tools/build_wine_orbis.sh
# gives the Wine PRXs (-soname NAME.so, -libname=NAME: create-fself names an
# importer's module and library after the NEEDED soname less ".so", an
# exporter's after -libname).
bind_ld() { # bind_ld NAME OUT OBJECTS...
    name=$1; elf=$2; shift 2
    ld.lld -m elf_x86_64 -pie --export-dynamic -e module_start --script "$OO_PS4_TOOLCHAIN/link.x" \
        -T "$root/wine/ps5/prx_eh_frame.ld" --eh-frame-hdr --no-rosegment -z max-page-size=0x4000 \
        -soname "$name.so" -z lazy --warn-unresolved-symbols -o "$elf" "$@" \
        -L"$OO_PS4_TOOLCHAIN/lib" -lc -lkernel 2> "$out/obj/$name.ld.log" ||
        { cat "$out/obj/$name.ld.log" >&2; fail "the binding module $name does not link"; }
}
bind_fself() { # bind_fself NAME [LIBRARY_DIR]
    (cd "$out" && "$fself" -in="$1.shared.elf" -out="$1.oelf" --lib="$1.prx" -libname="$1" \
        ${2:+-library-path="$2"} --paid 0x3800000000000011 > /dev/null) || fail "create-fself refused $1"
}
# bind_module NAME SOURCE MODE EXPORT...   MODE: lib | import | data | thunk
bind_module() {
    name=$1; source=$2; mode=$3; shift 3
    python3 "$root/tools/gen_prx_descriptor.py" --orbis "$out/obj/${name}_desc.c" "$@"
    "$cc" -std=c11 -O2 -Wall -Wextra -Werror -I"$root/wine/ps5" -c "$out/obj/${name}_desc.c" -o "$out/obj/${name}_desc.o"
    # -fPIC: the data import goes through the GOT (GLOB_DAT), not a copy relocation.
    # shellcheck disable=SC2086
    "$cc" $common -fPIC -c "$root/native/orbis/$source" -o "$out/obj/$name.o"
    objects="$out/obj/$name.o $out/obj/${name}_desc.o"
    case $mode in
    lib)
        # shellcheck disable=SC2086
        bind_ld "$name" "$out/$name.shared.elf" $objects
        bind_fself "$name" ;;
    import)
        # shellcheck disable=SC2086
        bind_ld "$name" "$out/obj/$name.first.elf" $objects
        python3 "$root/tools/orbis_import_stub.py" --first-link "$out/obj/$name.first.elf" \
            --exporter "$out/pw_bind_lib.shared.elf" --toolchain "$OO_PS4_TOOLCHAIN" \
            --out "$out/obj/${name}_stub.s" > /dev/null || fail "$name: no import stub"
        mkdir -p "$out/obj/$name.stubs"
        "$cc" -c "$out/obj/${name}_stub.s" -o "$out/obj/${name}_stub.o"
        ld.lld -m elf_x86_64 -shared -soname pw_bind_lib.so -o "$out/obj/$name.stubs/pw_bind_lib.so" "$out/obj/${name}_stub.o"
        # shellcheck disable=SC2086
        bind_ld "$name" "$out/$name.shared.elf" $objects "$out/obj/$name.stubs/pw_bind_lib.so"
        python3 "$root/tools/orbis_bind_table.py" --elf "$out/$name.shared.elf" \
            --exporter "pw_bind_lib=$out/obj/$name.stubs/pw_bind_lib.so" --out "$out/$name.bind.txt" --no-data > /dev/null ||
            fail "$name: no slot table"
        bind_fself "$name" "$out/obj/$name.stubs" ;;
    data)
        mkdir -p "$out/obj/$name.stubs"
        cp "$out/pw_bind_lib.shared.elf" "$out/obj/$name.stubs/pw_bind_lib.so"
        # shellcheck disable=SC2086
        bind_ld "$name" "$out/$name.shared.elf" $objects "$out/obj/$name.stubs/pw_bind_lib.so"
        llvm-readelf -r --wide "$out/$name.shared.elf" | grep -q 'R_X86_64_GLOB_DAT.* pw_bind_counter' ||
            fail "$name: pw_bind_counter is not a GOT (GLOB_DAT) import"
        python3 "$root/tools/orbis_bind_table.py" --elf "$out/$name.shared.elf" \
            --exporter "pw_bind_lib=$out/pw_bind_lib.shared.elf" --out "$out/$name.bind.txt" > /dev/null ||
            fail "$name: no slot table"
        bind_fself "$name" "$out/obj/$name.stubs" ;;
    thunk)
        python3 "$root/tools/gen_prx_imports.py" "$out/obj/${name}_imports.c" pw_bind_add pw_bind_name ||
            fail "$name: no thunks"
        "$cc" -std=c11 -O2 -Wall -Wextra -Werror -c "$out/obj/${name}_imports.c" -o "$out/obj/${name}_imports.o"
        # shellcheck disable=SC2086
        bind_ld "$name" "$out/$name.shared.elf" $objects "$out/obj/${name}_imports.o"
        ! llvm-readelf -d "$out/$name.shared.elf" | grep -q 'pw_bind_lib' || fail "$name names pw_bind_lib as NEEDED"
        bind_fself "$name" ;;
    esac
    llvm-readelf --dyn-syms "$out/$name.shared.elf" | grep -q 'GLOBAL DEFAULT .* module_start$' ||
        fail "$name: module_start is not a default-visibility export"
}
bind_module pw_bind_lib pw_bind_lib.c lib pw_bind_add pw_bind_name pw_bind_calls pw_bind_counter
bind_module pw_bind_use pw_bind_use.c import pw_bind_use_run
bind_module pw_bind_early pw_bind_use.c import pw_bind_use_run
bind_module pw_bind_data pw_bind_data.c data pw_bind_data_read pw_bind_data_address
bind_module pw_bind_thunk pw_bind_thunk.c thunk pw_bind_thunk_run pw_prx_bind_imports
echo "title: $out/eboot.elf (stub) + $out/ps4wine.prx ($build_id); probe modules: $out/pw_probe_mod.prx $out/pw_probe_tls.prx;" \
    "binding modules: pw_bind_lib pw_bind_use pw_bind_early pw_bind_data pw_bind_thunk"
echo "$out/eboot.elf"
