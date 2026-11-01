#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Runs a program under a host Wine whose win32u carries the PS5/PS4 user
# driver (Wine patch 0400, built with -DWINE_PS5_USER_DRIVER), with
# tests/wine_ps5_driver_sink.c preloaded as the title's present sink and
# input queue: every frame the driver presents goes to FRAMES/NNNN.ppm and
# the latest to FRAMES/last.ppm, and an optional scripted left click is
# made once AFTER frames have been shown. The program runs for SECONDS,
# then the prefix's wineserver is stopped. Prints the sink's frame count.
#
# Usage:
#   tools/test_wine_ps5_driver.sh [--build DIR] [--prefix DIR] [--frames DIR]
#       [--seconds N] [--click X,Y] [--after N] PROGRAM [ARGS...]
#
#   --build DIR    the host Wine build tree with the driver
#                  (default .deps/wine-host-drv/build: a copy of
#                  tools/build_host_wine.sh's tree with win32u rebuilt
#                  -DWINE_PS5_USER_DRIVER, see docs/ps4 HANDOFF-d in the
#                  PS4 port)
#   --prefix DIR   WINEPREFIX (default .deps/wine-host-drv/prefix; made if
#                  missing by wineboot of the plain host Wine,
#                  .deps/wine-host/install: under the driver build,
#                  wineboot's rundll32 InstallHinfSection never returns)
#   --frames DIR   where the frames go (default .deps/wine-host-drv/frames;
#                  emptied first)
#   --seconds N    how long the program runs (default 8)
#   --click X,Y    a left click at X,Y on the desktop (800x600)
#   --after N      the click comes once N frames were shown (default 1)
#   --preload LIB  one more library to preload after the sink (repeatable;
#                  tests/lab/pw_foreign_stack.c is one)
#   --no-frame-check  a program that presents nothing (hidden windows) is
#                  not a failure
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$root/.deps/wine-host-drv/build
prefix=$root/.deps/wine-host-drv/prefix
frames=$root/.deps/wine-host-drv/frames
seconds=8
click=
after=1
preload=
frame_check=1
fail() { echo "test_wine_ps5_driver: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case $1 in
    --build) build=$2; shift ;;
    --prefix) prefix=$2; shift ;;
    --frames) frames=$2; shift ;;
    --seconds) seconds=$2; shift ;;
    --click) click=$2; shift ;;
    --after) after=$2; shift ;;
    --preload) preload="$preload $2"; shift ;;
    --no-frame-check) frame_check= ;;
    -h|--help) sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --) shift; break ;;
    -*) fail "unknown argument $1" ;;
    *) break ;;
    esac
    shift
done
[ $# -ge 1 ] || fail "no program (see --help)"
[ -x "$build/wine" ] || fail "no host Wine build tree at $build"
nm "$build/dlls/win32u/win32u.so" 2>/dev/null | grep -q ps5_install_user_driver ||
    fail "$build/dlls/win32u/win32u.so has no user driver (rebuild it with -DWINE_PS5_USER_DRIVER)"
command -v cc >/dev/null || fail "cc is required"

mkdir -p "$frames" "$(dirname "$prefix")"
rm -f "$frames"/*.ppm
sink=$(dirname "$frames")/wine_ps5_driver_sink.so
cc -shared -fPIC -O2 -Wall -Wextra -o "$sink" "$root/tests/wine_ps5_driver_sink.c" -lpthread ||
    fail "the sink does not build"

# A caller's WINEDLLOVERRIDES (DXVK: d3d11,dxgi=n) is kept; winemenubuilder is always off.
export WINEPREFIX="$prefix" WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}winemenubuilder.exe=d"
init_wine=$root/.deps/wine-host/install/usr/bin/wine
if [ ! -f "$prefix/system.reg" ]; then
    [ -x "$init_wine" ] || fail "no plain host Wine at $init_wine to make the prefix"
    "$init_wine" wineboot --init >/dev/null 2>&1 || true
    "${init_wine}server" -w || true
    [ -f "$prefix/system.reg" ] || fail "wineboot made no prefix at $prefix"
fi
name=$(basename "$1")
# env: an optional NAME=VALUE from an expansion is no assignment to the shell.
env PW_SINK_PROCESS="${name%.exe}" PW_FRAME_DIR="$frames" PW_FRAME_DUMP="$frames/last.ppm" \
    PW_INPUT_AFTER="$after" ${click:+PW_INPUT_CLICK=$click} LD_PRELOAD="$sink$preload" \
    timeout -s KILL "$seconds" "$build/wine" "$@" > "$frames/run.log" 2>&1 || true
"$build/server/wineserver" -k 2>/dev/null || true
count=$(find "$frames" -name '[0-9]*.ppm' | wc -l)
echo "test_wine_ps5_driver: $count frame(s) of $name in $frames (log: $frames/run.log)"
[ -z "$frame_check" ] || [ "$count" -gt 0 ] || fail "no frame was presented"
