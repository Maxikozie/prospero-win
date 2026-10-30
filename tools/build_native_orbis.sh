#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The PS4 title: native/orbis/wine64_main_orbis.c with the start sequence,
# diagnostics and PRX descriptor code it shares with the PS5 title, compiled
# by tools/orbis-clang and linked as an OpenOrbis executable (the SDK's
# link.x and crt1.o, orbis-compat's corrections ahead of the SDK's libc).
# The packaging (create-fself --eboot, param.sfo, the .pkg) is the
# consumer's: this prints the ELF's path last.
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
# Environment: OO_PS4_TOOLCHAIN, ORBIS_COMPAT (include/ and build/), LLVM_BIN.
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
-I$root/include -I$root/src -I$root/native -I$root/native/orbis -I$root/wine/ps5 -I$root/wine/orbis \
-DPW_BUILD_ID=\"$build_id\" -DPW_ORBIS_NETLOG_HOST=\"$netlog_host\" -DPW_ORBIS_NETLOG_PORT=$netlog_port \
-DPW_ROOT=\"$data_root\""
sources="native/orbis/wine64_main_orbis.c native/orbis/orbislog/orbislog.c native/pw_diagnostics.c \
src/pw_result.c src/pw_wine_start.c wine/ps5/pw_wine_prx.c native/orbis/pw_stage_orbis.c \
native/orbis/pw_milestone_orbis.c native/orbis/pw_videoout_orbis.c native/orbis/pw_pad_orbis.c"
objects=""
for source in $sources; do
    object=$out/obj/$(basename "${source%.c}").o
    # shellcheck disable=SC2086
    "$cc" $common -c "$root/$source" -o "$object" || fail "cannot compile $source"
    objects="$objects $object"
done
# A raw syscall instruction outside libkernel kills the process.
if llvm-objdump -d "$out"/obj/*.o | grep -qw syscall; then fail "a raw syscall instruction in the title's objects"; fi

# shellcheck disable=SC2086
ld.lld -m elf_x86_64 -pie --script "$OO_PS4_TOOLCHAIN/link.x" --eh-frame-hdr \
    $objects -L"$compat_lib" -lorbis-compat "$compat_lib"/libc16/libc16_*.o \
    -L"$OO_PS4_TOOLCHAIN/lib" -lc -lkernel -lc++ -lSceNet -lSceSystemService -lSceUserService \
    -lSceVideoOut -lScePad \
    "$OO_PS4_TOOLCHAIN/lib/crt1.o" -o "$out/eboot.elf" --Map="$out/eboot.map" ||
    fail "the title does not link"

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
echo "title: $out/eboot.elf ($build_id); probe modules: $out/pw_probe_mod.prx $out/pw_probe_tls.prx;" \
    "binding modules: pw_bind_lib pw_bind_use pw_bind_early pw_bind_data pw_bind_thunk"
echo "$out/eboot.elf"
