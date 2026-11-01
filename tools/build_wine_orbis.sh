#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Wine's Unix side for the PS4 (Orbis OS, OpenOrbis toolchain): the pinned
# Wine with the console patch series, compiled by tools/orbis-clang and
# relinked into ntdll.prx, wineserver.prx and win32u.prx with the wine/ps5
# shims and the wine/orbis ones, each with a PRXDESC1 descriptor and the
# PS4 module parameter block. The structure follows tools/build_wine_ps5.sh;
# what differs is the toolchain (clang 18 + ld.lld at x86_64-pc-freebsd12-elf
# over the SDK and orbis-compat's overlay), the converter (create-fself
# --lib instead of ps5-native-tool), the exit wraps (a title may not exit)
# and report.json's binding check (the SDK's stub libraries instead of the
# PS5's libkernel_sys).
#
# Usage:
#   tools/build_wine_orbis.sh [--check-patches] [--jobs N] [--work DIR]
#       [--modules "ntdll wineserver win32u wowprospero [libvulkan winevulkan]"] [--allow-unbound]
#
# Environment:
#   OO_PS4_TOOLCHAIN       the OpenOrbis toolchain (required)
#   ORBIS_COMPAT           orbis-compat as consumers see it: include/ (the
#                          overlay) and build/ (liborbis-compat.a, libc16/)
#   PROSPERO_WINE_SOURCE   Wine checkout with the pin (default .deps/wine/source)
#   PROSPERO_WINE_BUILD    host tools tree with tools/winebuild (default
#                          .deps/wine/build, from tools/build_wine_runtime.sh)
#   PROSPERO_WINE_ORBIS_WORK  work directory (default .deps/wine-orbis)
#   LLVM_BIN               directory of clang/ld.lld/llvm-* 18 (default: PATH)
#   PROSPERO_RADV_ARCHIVE  mesa-ps4's libvulkan_radeon.a built with -femulated-tls
#                          (no native TLS: see wine/orbis/pw_emutls_orbis.c); when
#                          set, libvulkan.prx is built too (milestone e)
#   PROSPERO_RADV_ZLIB     the zlib archive of that build (libz.a)
#   PW_VULKAN_INCLUDE      the directory holding vulkan/vulkan.h (mesa-ps4's include/)
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir=${PROSPERO_WINE_SOURCE:-$root/.deps/wine/source}
host_tools=${PROSPERO_WINE_BUILD:-$root/.deps/wine/build}
work=${PROSPERO_WINE_ORBIS_WORK:-$root/.deps/wine-orbis}
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
modules="ntdll wineserver win32u wowprospero wow64native wineps5${PROSPERO_RADV_ARCHIVE:+ libvulkan winevulkan}"
allow_unbound=0
check_only=0

fail() { echo "build_wine_orbis: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case $1 in
    --check-patches) check_only=1 ;;
    --jobs) jobs=$2; shift ;;
    --work) work=$2; shift ;;
    --modules) modules=$2; shift ;;
    --allow-unbound) allow_unbound=1 ;;
    -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) fail "unknown argument $1" ;;
    esac
    shift
done

# One pin and one series: the console build's.
commit=$(sed -n 's/^WINE_COMMIT=//p' "$root/tools/build_wine_ps5.sh")
patches=$root/wine/patches
ordered=$(sh "$root/tools/build_wine_ps5.sh" --check-patches) || fail "the patch series is not well-formed"
if [ "$check_only" = 1 ]; then printf '%s\n' "$ordered"; exit 0; fi

: "${OO_PS4_TOOLCHAIN:?set OO_PS4_TOOLCHAIN}"
: "${ORBIS_COMPAT:?set ORBIS_COMPAT (include/ and build/)}"
[ -n "${LLVM_BIN:-}" ] && PATH="$LLVM_BIN:$PATH"
export PATH OO_PS4_TOOLCHAIN
export ORBIS_COMPAT_INCLUDE="$ORBIS_COMPAT/include"
compat_lib=$ORBIS_COMPAT/build
for tool in clang ld.lld llvm-ar llvm-nm llvm-objdump llvm-readelf python3 \
        i686-w64-mingw32-gcc x86_64-w64-mingw32-gcc; do
    command -v "$tool" >/dev/null || fail "$tool is required"
done
clang --version | grep -q 'clang version 18' || fail "clang 18 is required (LLVM_BIN)"
fself=$OO_PS4_TOOLCHAIN/bin/linux/create-fself
[ -x "$fself" ] || fself=$OO_PS4_TOOLCHAIN/bin/create-fself
[ -x "$fself" ] || fail "no create-fself under $OO_PS4_TOOLCHAIN/bin"
[ -f "$compat_lib/liborbis-compat.a" ] || fail "no liborbis-compat.a under $compat_lib"
[ -f "$ORBIS_COMPAT_INCLUDE/orbis_prefix.h" ] || fail "no orbis_prefix.h under $ORBIS_COMPAT_INCLUDE"
[ -x "$host_tools/tools/winebuild/winebuild" ] || fail "no host tools tree at $host_tools (tools/build_wine_runtime.sh)"
git -C "$source_dir" cat-file -e "$commit^{commit}" 2>/dev/null ||
    fail "$source_dir does not have the pinned Wine revision $commit"
cc=$root/tools/orbis-clang

mkdir -p "$work"
tree=$work/source
[ -d "$tree/.git" ] || git clone -q --shared --no-checkout "$source_dir" "$tree"
git -C "$tree" checkout -q --force "$commit"
git -C "$tree" clean -q -fdx
for patch in $ordered; do
    git -C "$tree" apply --index "$patches/$patch" || fail "patch does not apply: $patch"
done
python3 "$root/tools/stage_vk_batch.py" --source "$tree" --repo "$root" ||
    fail "cannot stage Vulkan command-stream runtime"
cp "$root/wine/ps5/time/pw_qpc_clock.h" "$tree/dlls/ntdll/pw_qpc_clock.h"
cp "$root/wine/ps5/menu/pw_title_menu.h" "$tree/dlls/ntdll/pw_title_menu.h"
cp "$root/wine/ps5/input/pw_key_shared.h" "$tree/dlls/win32u/pw_key_shared.h"

# FreeType draws Wine's text (milestone d3): the release, units and exports
# tools/build_wine_ps5.sh pins, compiled by tools/orbis-clang into a static
# archive for configure and the objects of libfreetype.prx, which win32u's
# dlopen("libfreetype.so") loads through pw_wine_dl beside ntdll.prx.
# PROSPERO_FREETYPE_TARBALL names a local copy of the tarball.
eval "$(sed -n '/^FREETYPE_VERSION=/,/^ FT_Set_Pixel_Sizes FT_Vector_Length FT_Vector_Transform FT_Vector_Unit"$/p' \
    "$root/tools/build_wine_ps5.sh")"
[ -n "${FREETYPE_SHA256:-}" ] && [ -n "${FREETYPE_EXPORTS:-}" ] || fail "cannot read the FreeType pin from tools/build_wine_ps5.sh"
ft=$work/freetype
ft_stamp=$(printf '%s\n' "$FREETYPE_SHA256" "$FREETYPE_UNITS" "$(cat "$cc")" | sha256sum | cut -c1-64)
if [ "$(cat "$ft/.orbis-stamp" 2>/dev/null)" != "$ft_stamp" ]; then
    tarball=${PROSPERO_FREETYPE_TARBALL:-$work/freetype-$FREETYPE_VERSION.tar.xz}
    [ -f "$tarball" ] || curl -sSfL -o "$tarball" "$FREETYPE_URL" || fail "cannot download $FREETYPE_URL"
    [ "$(sha256sum "$tarball" | cut -c1-64)" = "$FREETYPE_SHA256" ] ||
        fail "$tarball does not match FreeType $FREETYPE_VERSION's pinned SHA-256"
    rm -rf "$ft"
    mkdir -p "$ft/src" "$ft/obj" "$ft/config"
    tar -xJf "$tarball" -C "$ft/src" --strip-components=1
    cat > "$ft/config/ftmodule_orbis.h" <<'EOF'
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, t1_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, winfnt_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
EOF
    for unit in $FREETYPE_UNITS; do
        "$cc" -std=c99 -O2 -fPIC -DFT2_BUILD_LIBRARY '-DFT_CONFIG_MODULES_H=<ftmodule_orbis.h>' \
            -I"$ft/config" -I"$ft/src/include" -c "$ft/src/src/$unit.c" -o "$ft/obj/$(basename "$unit").o" \
            > "$ft/obj/$(basename "$unit").log" 2>&1 || fail "cannot compile FreeType $unit.c (see $ft/obj)"
    done
    llvm-ar rcs "$ft/libfreetype.a" "$ft"/obj/*.o
    echo "$ft_stamp" > "$ft/.orbis-stamp"
    echo "built FreeType $FREETYPE_VERSION"
fi

# Configure for a FreeBSD 12 host (the target triple's OS: Wine takes its
# FreeBSD signal, thread and server code) without the desktop libraries;
# FreeType is found by its flags (its soname is the name win32u dlopens);
# so is Vulkan (milestone e): win32u dlopens "libvulkan.so", which pw_wine_dl
# loads as libvulkan.prx beside ntdll.prx, and winevulkan is built.
configure_args="--host=x86_64-unknown-freebsd12 --build=x86_64-pc-linux-gnu \
--enable-archs=i386,x86_64 --disable-tests --without-x \
--without-fontconfig --without-alsa --without-pulse \
--without-dbus --without-gstreamer --without-sdl --without-udev --without-usb \
--without-v4l2 --without-wayland --without-opengl --without-oss \
--without-pcap --without-pcsclite --without-sane --without-krb5 --without-gphoto \
--without-netapi --without-inotify --without-capi --without-cups --without-gssapi \
--without-ffmpeg --without-gnutls --without-unwind \
ac_cv_func_thr_kill2=yes $(for h in capi cdrom errqueue filter fs hdreg hidraw input ioctl ipx irda major \
memfd ntsync param rtnetlink serial types ucdrom userfaultfd videodev2 wireless; do \
printf 'ac_cv_header_linux_%s_h=no ' "$h"; done) \
ac_cv_header_sys_epoll_h=no wine_cv_have_sched_setaffinity=no $(for f in epoll_create sched_getcpu getrandom \
sysinfo memfd_create process_vm_readv process_vm_writev futimesat; do printf 'ac_cv_func_%s=no ' "$f"; done)"
# The SDK ships musl's Linux headers beside Sony's FreeBSD kernel: a
# linux/*.h that configure finds would select Linux code (seccomp filters,
# memfd) the kernel does not have, so every linux/ header is declared absent.
# thr_kill2 is not in libkernel: wine/ps5/pw_wine_threads_libc.c provides it
# at the PRX link (configure's link test cannot see that), and without
# HAVE_THR_KILL2 the server would signal threads with kill().
# The SDK's libc.a is musl with the system-call layer replaced: the functions
# above exist in it as stubs that set ENOSYS, so configure's link test finds
# them and Wine takes code that always fails (HAVE_EPOLL_CREATE put the
# server's main loop on epoll, which returned -1 at once; the loop then
# polled, unseen). tools/orbis_libc_stubs.py lists every such stub and the
# check after configure refuses a config.h that takes one for real; sys/epoll.h
# is musl's header for the same stubs.
cflags=${CFLAGS:--g -O2}
stamp=$({ printf '%s\n' "$commit" "$configure_args" "$cflags" "$OO_PS4_TOOLCHAIN" "$ft_stamp"
          cat "$cc" "$root"/wine/orbis/include/*.h "$root"/wine/orbis/include/*/*.h
          for patch in $ordered; do cat "$patches/$patch"; done; } | sha256sum | cut -c1-64)
build=$work/build
if [ ! -f "$build/Makefile" ] || [ "$(cat "$build/.orbis-stamp" 2>/dev/null)" != "$stamp" ]; then
    rm -rf "$build"
    mkdir -p "$build"
    # shellcheck disable=SC2086
    (cd "$build" && "$tree/configure" $configure_args CC="$cc" CFLAGS="$cflags" \
        FREETYPE_CFLAGS="-I$ft/src/include" FREETYPE_LIBS="$ft/libfreetype.a" \
        ac_cv_lib_soname_freetype=libfreetype.so ac_cv_lib_soname_vulkan=libvulkan.so \
        --with-wine-tools="$host_tools" > "$work/configure.log" 2>&1) ||
        fail "configure failed; see $work/configure.log"
    echo "$stamp" > "$build/.orbis-stamp"
fi
python3 "$root/tools/orbis_libc_stubs.py" --config "$build/include/config.h" ||
    fail "configure took an ENOSYS stub of the SDK's libc for a real function; declare it absent in configure_args"
grep -q '^#define SONAME_LIBFREETYPE "libfreetype.so"' "$build/include/config.h" ||
    fail "configure did not take FreeType (see $work/configure.log): win32u would draw no text"
grep -q '^#define SONAME_LIBVULKAN "libvulkan.so"' "$build/include/config.h" ||
    fail "configure did not take Vulkan (see $work/configure.log): win32u would have no Vulkan driver"

# The shims, compiled once: the heap and direct memory (ntdll's ELF link
# takes them), then the loader, compat, cwd, threads, sink and exit units.
obj=$work/obj
mkdir -p "$obj"
shim_cflags="-std=gnu11 -O2 -Wall -Wextra -Werror"
compile_shim() {
    # shellcheck disable=SC2086
    "$cc" $shim_cflags -I"$root/wine/ps5" -I"$root/wine/orbis" -c "$1" -o "$obj/$(basename "${1%.c}").o" ||
        fail "cannot compile $1"
}
for unit in pw_wine_heap pw_wine_heap_libc pw_wine_dmem pw_wine_dmem_ps5 \
        pw_wine_prx pw_wine_dl pw_wine_dl_libc pw_wine_compat pw_wine_compat_libc \
        pw_wine_threads pw_wine_threads_libc pw_wine_sink pw_wine_cwd pw_wine_cwd_libc; do
    compile_shim "$root/wine/ps5/$unit.c"
done
for unit in pw_wine_exit_orbis pw_wine_compat_orbis pw_wine_env_orbis pw_wine_stdio_orbis pw_wine_dl_orbis pw_wine_dup_orbis \
        pw_wine_threads_orbis; do
    compile_shim "$root/wine/orbis/$unit.c"
done
case " $modules " in *" libvulkan "*)
    # libvulkan.prx's own units: the entry points and present counters over
    # RADV, and the emulated-TLS runtime RADV's -femulated-tls build calls.
    [ -f "${PROSPERO_RADV_ARCHIVE:-}" ] || fail "libvulkan needs PROSPERO_RADV_ARCHIVE (mesa-ps4 built with -femulated-tls)"
    [ -f "${PROSPERO_RADV_ZLIB:-}" ] || fail "libvulkan needs PROSPERO_RADV_ZLIB (the zlib archive of that build)"
    [ -f "${PW_VULKAN_INCLUDE:-}/vulkan/vulkan.h" ] || fail "libvulkan needs PW_VULKAN_INCLUDE (vulkan/vulkan.h)"
    "$cc" $shim_cflags -I"$root/wine/orbis" -I"$PW_VULKAN_INCLUDE" -c "$root/wine/orbis/pw_vulkan_radv_orbis.c" \
        -o "$obj/pw_vulkan_radv_orbis.o" || fail "cannot compile pw_vulkan_radv_orbis.c"
    ;;
esac
# The emulated-TLS runtime: RADV's -femulated-tls build (libvulkan) and
# wowprospero's unix.c call it.
compile_shim "$root/wine/orbis/pw_emutls_orbis.c"
heap="$obj/pw_wine_heap.o $obj/pw_wine_heap_libc.o"
dmem="$obj/pw_wine_dmem.o $obj/pw_wine_dmem_ps5.o"

# The ELF links: Wine's own make, with the heap and dmem objects on ntdll's
# link line and the server's own heap on its. Unresolved symbols are warnings
# here (the PRX link reports them); the make log keeps every link command
# line, which the PRX link reads back.
: > "$work/make.log"
status=0
for step in "dlls/ntdll/ntdll.so|$heap $dmem" "dlls/win32u/win32u.so|" "server/wineserver|$heap" \
        "dlls/winevulkan/winevulkan.so|"; do
    target=${step%%|*}; objects=${step#*|}
    case " $modules " in *" $(basename "${target%.so}") "*) ;; *) continue ;; esac
    (cd "$build" && rm -f "$target")
    make -C "$build" -k -j"$jobs" LDFLAGS="$objects -Wl,--warn-unresolved-symbols" "$target" \
        >> "$work/make.log" 2>&1 || status=$?
done
[ "$status" = 0 ] || echo "build_wine_orbis: make returned $status; see $work/make.log" >&2

# The PRX link.
prx=$work/prx
rm -rf "$prx"
mkdir -p "$prx/obj" "$prx/sce_module"
link_objects() {
    python3 - "$work/make.log" "$1" <<'PY'
import shlex, sys
text = open(sys.argv[1], errors="replace").read().replace("\\\n", " ")
lines = [line for line in text.splitlines() if f"-o {sys.argv[2]} " in line]
if lines:
    print(" ".join(arg for arg in shlex.split(lines[-1])
                   if arg.endswith(".o") and not arg.startswith("/")))
PY
}
cwd_wraps=""
for name in $(grep -o '__wrap_[a-z_]*' "$root/wine/ps5/pw_wine_cwd_libc.c" | sed 's/^__wrap_//' |
        LC_ALL=C sort -u); do
    cwd_wraps="$cwd_wraps --wrap=$name"
done
exit_wraps="--wrap=exit --wrap=_exit --wrap=abort --wrap=write --wrap=writev --wrap=_write --wrap=_writev --wrap=dup --wrap=fcntl --wrap=pipe"
# link_prx NAME TARGET "EXTRA OBJECTS": a position-independent image laid
# out by the SDK's own linker script (create-fself reads that layout; an
# ld.lld --shared image crashes it), every global exported, entry
# module_start, the SDK's static libc behind orbis-compat's corrections,
# libkernel and the system stubs as needed. Symbols defined in the image bind
# locally, as -Bsymbolic would. Then the host gates and the conversion to a
# PRX. The recipe is the one the milestone a probe loaded and resolved on
# the console, plus --no-rosegment and the console's 16 KiB page: the PS4
# loader maps raw p_vaddr and rounds segment sizes to a page, so a PT_LOAD
# that starts mid-page is refused with ENOEXEC (0x80020008, measured on
# ntdll.prx, whose .data/.bss followed a read-only module-parameter segment
# by 0x20 bytes; the block is writable now, see gen_prx_descriptor.py).
# --no-rosegment keeps lld from making a separate read-only segment of the
# rodata as orbis-porting-kit does for executables.
# A module that imports from a sibling PRX (win32u from ntdll) names the
# exporter as its 4th argument (several, comma-separated in priority order:
# winevulkan's "ntdll,win32u"). A first link shows what the module cannot
# get elsewhere (Wine's own symbols, the exporter's __wrap_* hooks);
# tools/orbis_import_stub.py turns that, plus the dlfcn and malloc families
# (the importer's heap and look-ups are the exporter's, as on the PS5), into
# a stub library with the exporter's soname, and the second link binds those
# names to it ahead of the static libc. Only those: binding against the
# exporter's whole dynamic table also took libc's TLS variable from ntdll.
# create-fself turns them into imports of the exporter's library. The names
# must agree: create-fself names an importer's module and library after the
# NEEDED soname less ".so", and an exporter's after -libname, hence -soname
# NAME.so and -libname=NAME on every module, and the stub under that soname
# in $prx/stubs. tools/orbis_bind_table.py then lists the import slots for
# the title's check. Measured on the host only: whether the console loader
# binds them is milestone d1's question.
import_also="malloc,free,calloc,realloc,posix_memalign,aligned_alloc,memalign"
# libkernel exports dlopen/dlsym/dlclose/dlerror too, and create-fself gives
# an import to the first NEEDED library exporting its name (libkernel first;
# measured in d1: win32u's dlsym slot held libkernel's). An importer takes
# ntdll's dlfcn under the __wrap_ names of wine/orbis/pw_wine_dl_orbis.c.
import_wraps="--wrap=dlopen --wrap=dlsym --wrap=dlclose --wrap=dlerror --wrap=dladdr"
prx_status=0
mkdir -p "$prx/stubs"
ld_prx() { # ld_prx NAME OUT ARGS...
    name=$1; out=$2; shift 2
    # shellcheck disable=SC2086
    (cd "$build" && ld.lld -m elf_x86_64 -pie --export-dynamic -e module_start \
        --script "$OO_PS4_TOOLCHAIN/link.x" -T "$root/wine/ps5/prx_eh_frame.ld" --eh-frame-hdr \
        --no-rosegment -z max-page-size=0x4000 -soname "$name.so" -z lazy \
        --warn-unresolved-symbols -o "$out" "$@" \
        -L"$compat_lib" -lorbis-compat "$compat_lib"/libc16/libc16_*.o \
        -L"$OO_PS4_TOOLCHAIN/lib" -lc -lkernel -lc++ -lSceNet --as-needed "$OO_PS4_TOOLCHAIN"/lib/libSce*.so \
        --no-as-needed)
}
link_prx() {
    name=$1; objects=; exporters=$(printf '%s' "${4:-}" | tr ',' ' ')
    [ "$2" = - ] || objects=$(link_objects "$2")
    log=$prx/$name.link.log
    [ -n "$objects$3" ] || { echo "no ELF link of $2 in make.log" > "$log"; prx_status=1; return; }
    : > "$log"
    bind_args=
    if [ -n "$exporters" ]; then
        # One stub per exporter (winevulkan: ntdll, then win32u), in the
        # order given: NEEDED follows it, and a name goes to the first
        # exporter that defines it.
        stub_args=
        for exporter in $exporters; do stub_args="$stub_args --exporter $exporter=$prx/$exporter.shared.elf"; done
        # shellcheck disable=SC2086
        ld_prx "$name" "$prx/obj/$name.first.elf" $objects $3 >> "$log" 2>&1 || { prx_status=1; return; }
        # shellcheck disable=SC2086
        python3 "$root/tools/orbis_import_stub.py" --first-link "$prx/obj/$name.first.elf" \
            $stub_args --toolchain "$OO_PS4_TOOLCHAIN" --also "$import_also" \
            --out "$prx/obj/${name}_{exporter}_stub.s" --list "$prx/$name.imports.txt" >> "$log" 2>&1 ||
            { prx_status=1; return; }
        for exporter in $exporters; do
            "$cc" -c "$prx/obj/${name}_${exporter}_stub.s" -o "$prx/obj/${name}_${exporter}_stub.o" >> "$log" 2>&1 &&
                ld.lld -m elf_x86_64 -shared -soname "$exporter.so" -o "$prx/obj/$name.$exporter.stub.so" \
                    "$prx/obj/${name}_${exporter}_stub.o" >> "$log" 2>&1 || { prx_status=1; return; }
            objects="$objects $prx/obj/$name.$exporter.stub.so"
            bind_args="$bind_args --exporter $exporter=$prx/obj/$name.$exporter.stub.so"
        done
    fi
    # shellcheck disable=SC2086
    ld_prx "$name" "$prx/$name.shared.elf" $objects $3 >> "$log" 2>&1 || { prx_status=1; return; }
    llvm-readelf --dyn-syms "$prx/$name.shared.elf" | grep -q 'GLOBAL DEFAULT .* module_start$' ||
        { echo "module_start is not a default-visibility export" >> "$log"; prx_status=1; return; }
    if [ -n "$exporters" ]; then
        # shellcheck disable=SC2086
        python3 "$root/tools/orbis_bind_table.py" --elf "$prx/$name.shared.elf" \
            $bind_args --out "$prx/sce_module/$name.bind.txt" \
            --no-data >> "$log" 2>&1 || { prx_status=1; return; }
        for exporter in $exporters; do cp "$prx/obj/$name.$exporter.stub.so" "$prx/stubs/$exporter.so"; done
    fi
    (cd "$prx" && "$fself" -in="$name.shared.elf" -out="$name.oelf" --lib="sce_module/$name.prx" \
        -libname="$name" -library-path="$prx/stubs" --paid 0x3800000000000011) >> "$log" 2>&1 ||
        { prx_status=1; return; }
    for exporter in $exporters; do rm -f "$prx/stubs/$exporter.so"; done
    chmod 755 "$prx/sce_module/$name.prx"
}
desc() {
    name=$1; shift
    python3 "$root/tools/gen_prx_descriptor.py" --orbis "$prx/obj/${name}_desc.c" "$@" ||
        fail "cannot generate the $name descriptor"
    # shellcheck disable=SC2086
    "$cc" -std=c11 -O2 -Wall -Wextra -Werror -I"$root/wine/ps5" -c "$prx/obj/${name}_desc.c" \
        -o "$prx/obj/${name}_desc.o" || fail "cannot compile the $name descriptor"
}
case " $modules " in *" ntdll "*)
    desc ntdll __wine_main pw_wine_dl_adopt dlopen dlsym dlerror \
        pw_wine_heap_stats pw_wine_heap_malloc pw_wine_heap_free \
        pw_wine_set_present_sink pw_wine_post_input pw_wine_sink_stats pw_wine_set_flush_sink pw_wine_note_flush \
        __wine_prospero_native_wow64_caps \
        pw_wine_present pw_wine_next_input pw_wine_input_fd \
        pw_wine_set_pad pw_wine_rumble pw_wine_pad pw_wine_set_rumble pw_wine_set_menu pw_wine_menu \
        pw_wine_set_pad_at pw_wine_rumble_at pw_wine_pad_at pw_wine_set_rumble_at pw_wine_pads_wanted \
        pw_wine_set_audio_sink pw_wine_audio_available pw_wine_audio_output \
        __wine_virtual_stats __wine_virtual_fault_top __wine_ps5_set_output_sink __wine_ps5_memory_stats pw_cwd_set \
        __wine_ps5_set_segv_hook __wine_ps5_set_segv_unresolved_hook pw_wine_set_display_release pw_wine_release_display \
        pw_wine_set_exit_hook pw_wine_setenv pw_wine_thread_stats
    shims=""
    for unit in pw_wine_prx pw_wine_dl pw_wine_dl_libc pw_wine_compat pw_wine_compat_libc \
            pw_wine_compat_orbis pw_wine_sink pw_wine_cwd pw_wine_cwd_libc pw_wine_exit_orbis pw_wine_env_orbis pw_wine_stdio_orbis \
            pw_wine_dl_orbis pw_wine_dup_orbis pw_wine_threads_orbis; do
        shims="$shims $obj/$unit.o"
    done
    link_prx ntdll dlls/ntdll/ntdll.so "$heap $dmem $shims $prx/obj/ntdll_desc.o $cwd_wraps $exit_wraps"
    ;;
esac
case " $modules " in *" wineserver "*)
    desc wineserver pw_wineserver_connect pw_wine_thread_register pw_wine_thread_unregister \
        pw_wineserver_call_direct pw_wineserver_try_fast_mutex pw_wineserver_mutex_backend \
        pw_wineserver_sync_backend pw_cwd_share_changes pw_wine_set_exit_hook pw_wine_setenv
    link_prx wineserver server/wineserver "$heap $obj/pw_wine_compat.o $obj/pw_wine_compat_libc.o \
        $obj/pw_wine_compat_orbis.o $obj/pw_wine_dup_orbis.o $obj/pw_wine_threads.o $obj/pw_wine_threads_libc.o \
        $obj/pw_wine_cwd.o $obj/pw_wine_cwd_libc.o $obj/pw_wine_exit_orbis.o $obj/pw_wine_env_orbis.o $obj/pw_wine_stdio_orbis.o \
        $prx/obj/wineserver_desc.o $cwd_wraps $exit_wraps"
    ;;
esac
case " $modules " in *" win32u "*)
    # Its ntdll imports are loader imports of ntdll.prx (see link_prx).
    [ -f "$prx/ntdll.shared.elf" ] || fail "win32u imports from ntdll: build ntdll in the same run"
    # pw_wine_setenv: ntdll's loader replays Wine's environment into a module
    # that exports it, and win32u's user driver reads WINE_PS5_DESKTOP and
    # WINE_PS5_VIEW there (measured in f: without it the plan's 960x540 was
    # never seen and the desktop stayed 800x600).
    desc win32u __wine_unix_lib_init pw_wine_setenv
    # win32u opens and maps font files itself (freetype.c unix_face_create,
    # map_font_file) by .../dosdevices/c:/... names, so it links its own copy
    # of the working-directory emulation too (measured in d2: with the
    # listing fixed, all 13 fonts reached unix_face_create and none loaded).
    # shellcheck disable=SC2086
    link_prx win32u dlls/win32u/win32u.so "-u opendir $prx/obj/win32u_desc.o $obj/pw_wine_cwd.o $obj/pw_wine_cwd_libc.o \
        $obj/pw_wine_env_orbis.o $exit_wraps $import_wraps $cwd_wraps" ntdll
    # FreeType, which win32u loads by name, with its own static libc; the
    # exit and stdio wraps are ntdll's (it may not exit, its stderr reaches
    # the title). It opens font files with its own fopen, by the names
    # win32u passes (.../dosdevices/c:/windows/Fonts/...), so it links its
    # own copy of the working-directory emulation, as wineserver does: the
    # c: link exists only in that emulation's table (measured in d2:
    # AddFontResourceEx of tahoma.ttf failed with the file in place). Its
    # only reference to opendir is the emulation's __real_opendir, which
    # ld.lld bound to libSceLibcInternal's export instead of fetching
    # libc.a's member (that library then became NEEDED, and it exports the
    # malloc family the stub takes from ntdll): -u opendir fetches it.
    # shellcheck disable=SC2086
    desc libfreetype $FREETYPE_EXPORTS
    link_prx libfreetype - "-u opendir $ft/obj/*.o $prx/obj/libfreetype_desc.o $obj/pw_wine_cwd.o \
        $obj/pw_wine_cwd_libc.o $exit_wraps $cwd_wraps" ntdll
    # Wine's own fonts, staged under share/wine/fonts and copied into the
    # prefix's Fonts directory by scripts/ps4/prefix.sh (an /app0 or
    # unlisted directory is not searched).
    mkdir -p "$prx/fonts"
    cp "$tree"/fonts/*.ttf "$prx/fonts/"
    ;;
esac

case " $modules " in *" winevulkan "*)
    # winevulkan's Unix side (milestone e): Wine's Vulkan thunks, which take
    # the driver from win32u (__wine_get_vulkan_driver, its one import from
    # there) and everything else from ntdll; loaded by ntdll as the PE
    # winevulkan.dll's unix library.
    [ -f "$prx/ntdll.shared.elf" ] && [ -f "$prx/win32u.shared.elf" ] ||
        fail "winevulkan imports from ntdll and win32u: build both in the same run"
    desc winevulkan __wine_unix_call_funcs __wine_unix_call_wow64_funcs
    # shellcheck disable=SC2086
    link_prx winevulkan dlls/winevulkan/winevulkan.so "$prx/obj/winevulkan_desc.o $exit_wraps $import_wraps" \
        ntdll,win32u
    ;;
esac
case " $modules " in *" libvulkan "*)
    # RADV (milestone e), which win32u's dlopen("libvulkan.so") loads through
    # pw_wine_dl beside ntdll.prx: module_start runs its constructors (ACO's
    # opcode tables), then ntdll's environment is replayed into its own
    # (pw_wine_setenv: RADV reads ORBIS_*, MESA_* and RADV_* there). Its exit
    # and stdio wraps, dlfcn and malloc family are ntdll's, as libfreetype's.
    # The archive is whole: Mesa's entry points are reached through lookup
    # tables, not by name.
    [ -f "$prx/ntdll.shared.elf" ] || fail "libvulkan imports from ntdll: build ntdll in the same run"
    desc libvulkan vkGetInstanceProcAddr vkGetDeviceProcAddr pw_vulkan_stats pw_wine_setenv
    # shellcheck disable=SC2086
    link_prx libvulkan - "$prx/obj/libvulkan_desc.o $obj/pw_vulkan_radv_orbis.o $obj/pw_emutls_orbis.o \
        $obj/pw_wine_env_orbis.o --whole-archive $PROSPERO_RADV_ARCHIVE --no-whole-archive $PROSPERO_RADV_ZLIB \
        $exit_wraps $import_wraps" ntdll
    ;;
esac

case " $modules " in *" wowprospero "*)
    # wowprospero (milestone f): the WoW64 CPU backend's Unix side around the
    # IA-32 translator (the PS5 recipe's units, tools/build_wine_ps5.sh),
    # loaded by ntdll as the PE wowprospero.dll's unix library (pw_wine_dl:
    # x86_64-unix/wowprospero.so becomes .prx). unix.c's one __thread is
    # emulated (-femulated-tls, pw_emutls_orbis: native TLS is broken in an
    # app PRX); its executable memory is Wine's (NtAllocateVirtualMemoryEx,
    # direct memory under patch 0600) and its fault handling ntdll's hooks
    # (0610, 0710), all imports from ntdll. ntdll's environment is replayed
    # into its own (pw_wine_setenv: PW_WOW_*). pw_wow_stats is for the title.
    [ -f "$prx/ntdll.shared.elf" ] || fail "wowprospero imports from ntdll: build ntdll in the same run"
    mkdir -p "$obj/wowprospero"
    wow_objects=
    for unit in wine/wowprospero/unix.c src/pw_x86_engine.c src/pw_x86_block.c src/pw_x86_cache.c \
            src/pw_x86_hostexec.c src/pw_x86_reencode.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c; do
        wow_tls=
        [ "$unit" != wine/wowprospero/unix.c ] || wow_tls=-femulated-tls
        # shellcheck disable=SC2086
        "$cc" -std=gnu11 -O2 -Wall $wow_tls -D__WINESRC__ -DWINE_UNIX_LIB -D_REENTRANT -I"$root/wine/wowprospero" \
            -I"$build/include" -I"$tree/include" -I"$root/src" -I"$root/include" -c "$root/$unit" \
            -o "$obj/wowprospero/$(basename "${unit%.c}").o" || fail "cannot compile $unit"
        wow_objects="$wow_objects $obj/wowprospero/$(basename "${unit%.c}").o"
    done
    # A native __thread would link without an import (lld relaxes it to
    # local-exec, wrong in an app PRX), so the report cannot see it: check.
    llvm-nm "$obj/wowprospero/unix.o" | grep -q ' U __emutls_get_address$' ||
        fail "wowprospero's unix.c keeps a native __thread (needs -femulated-tls)"
    desc wowprospero __wine_unix_call_funcs pw_wow_stats pw_wine_setenv
    # shellcheck disable=SC2086
    link_prx wowprospero - "$prx/obj/wowprospero_desc.o $wow_objects $obj/pw_emutls_orbis.o $obj/pw_wine_env_orbis.o \
        $exit_wraps $import_wraps" ntdll
    ;;
esac
case " $modules " in *" wineps5 "*)
    # The audio driver (wine/wineps5, phase p5): mmdevapi loads its unix
    # library by the name patch 0120 gives the console's driver ("ps5":
    # wineps5.so, a PRX here), and it mixes every stream into one 256-frame
    # grain for the title's audio sink, which ntdll exports
    # (pw_wine_audio_output). The PS5 recipe (tools/build_wine_ps5.sh): its
    # unix.c and the platform-free mixer. mmdevapi's IDL headers first.
    [ -f "$prx/ntdll.shared.elf" ] || fail "wineps5 imports from ntdll: build ntdll in the same run"
    make -C "$build" include/audioclient.h include/mmdeviceapi.h include/devicetopology.h \
        include/propsys.h include/propidl.h include/objidl.h include/objidlbase.h \
        include/unknwn.h include/wtypes.h include/oaidl.h >> "$work/make.log" 2>&1 ||
        fail "cannot generate mmdevapi's headers; see $work/make.log"
    mkdir -p "$obj/wineps5"
    audio_objects=
    for unit in wine/wineps5/unix.c src/pw_audio_mix.c; do
        "$cc" -std=gnu11 -O2 -Wall -D__WINESRC__ -DWINE_UNIX_LIB -D_REENTRANT -I"$build/include" -I"$tree/include" \
            -I"$tree/dlls/mmdevapi" -I"$root/wine/ps5" -I"$root/src" -I"$root/include" -c "$root/$unit" \
            -o "$obj/wineps5/$(basename "${unit%.c}").o" || fail "cannot compile $unit"
        audio_objects="$audio_objects $obj/wineps5/$(basename "${unit%.c}").o"
    done
    desc wineps5 __wine_unix_call_funcs __wine_unix_call_wow64_funcs
    # shellcheck disable=SC2086
    link_prx wineps5 - "$prx/obj/wineps5_desc.o $audio_objects $exit_wraps $import_wraps" ntdll
    ;;
esac
case " $modules " in *" wow64native "*)
    # The hardware WoW64 backend's Unix side (wine/wow64native/unix.c), loaded
    # by ntdll as the PE wow64native.dll's unix library like wowprospero. It
    # enables itself only when ntdll's __wine_prospero_native_wow64_caps
    # (patch 0883, on the PS4 through 1090) reports all three bits; its
    # __thread state is emulated as wowprospero's is. Its environment comes
    # from ntdll's (pw_wine_setenv: PW_NATIVE_PROFILE).
    [ -f "$prx/ntdll.shared.elf" ] || fail "wow64native imports from ntdll: build ntdll in the same run"
    mkdir -p "$obj/wow64native"
    "$cc" -std=gnu11 -O2 -Wall -femulated-tls -D__WINESRC__ -DWINE_UNIX_LIB -D_REENTRANT -I"$root/wine/wow64native" \
        -I"$build/include" -I"$tree/include" -c "$root/wine/wow64native/unix.c" -o "$obj/wow64native/unix.o" ||
        fail "cannot compile wine/wow64native/unix.c"
    llvm-nm "$obj/wow64native/unix.o" | grep -q ' U __emutls_get_address$' ||
        fail "wow64native's unix.c keeps a native __thread (needs -femulated-tls)"
    desc wow64native __wine_unix_call_funcs pw_wine_setenv
    # shellcheck disable=SC2086
    link_prx wow64native - "$prx/obj/wow64native_desc.o $obj/wow64native/unix.o $obj/pw_emutls_orbis.o \
        $obj/pw_wine_env_orbis.o $exit_wraps $import_wraps" ntdll
    ;;
esac

must_bind=$(printf '%s' "$modules" | tr ' ' ',')
report_status=0
python3 "$root/tools/orbis_prx_report.py" --work "$work" --toolchain "$OO_PS4_TOOLCHAIN" \
    --out "$work/report.json" --wine-commit "$commit" --patches "$patches" \
    --must-bind "$must_bind" || report_status=$?
echo "report: $work/report.json"
for name in $modules; do
    if [ -f "$prx/sce_module/$name.prx" ]; then
        echo "  $name.prx: $(wc -c < "$prx/sce_module/$name.prx") bytes"
    else
        echo "  $name.prx: NOT BUILT (see $prx/$name.link.log)"
    fi
done
[ "$prx_status" = 0 ] || fail "a PRX link failed; see $prx/*.link.log"
[ "$report_status" = 0 ] || [ "$allow_unbound" = 1 ] || fail "report.json has errors"
echo "wine orbis: $prx/sce_module"
