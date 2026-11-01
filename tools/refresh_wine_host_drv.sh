#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Refreshes .deps/wine-host-drv, the host Wine whose win32u carries the
# console user driver (patch 0400 with -DWINE_PS5_USER_DRIVER, and 0461's
# headless Vulkan surface with -DWINE_PS5_HEADLESS_SURFACE), after
# tools/build_host_wine.sh rebuilt its source tree:
#   1. the build tree is copied from the host build (rsync --delete);
#   2. win32u's Unix side is rebuilt with the two defines;
#   3. the driver's prefix is updated by the plain build-tree Wine of the
#      host build: Wine updates a prefix when wine.inf is newer than its
#      stamp, and wineboot never returns under the driver build.
# tools/test_wine_ps5_driver.sh runs programs under the result. Anything
# built into the host tree (dlls/wowprospero from build_wowprospero.sh)
# comes along with the copy.
#
# Usage:
#   tools/refresh_wine_host_drv.sh [--host DIR] [--jobs N] [--no-prefix]
#
#   --host DIR    the host Wine build tree (default .deps/wine-host/build)
#   --jobs N      make -j for win32u (default: nproc)
#   --no-prefix   leave .deps/wine-host-drv/prefix as it is
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
host=$root/.deps/wine-host/build
drv=$root/.deps/wine-host-drv
jobs=$(nproc 2>/dev/null || echo 4)
update_prefix=1

fail() { echo "refresh_wine_host_drv: $*" >&2; exit 1; }
usage() { sed -n '3,/^set -eu/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case $1 in
    -h|--help) usage; exit 0 ;;
    --host) host=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    --no-prefix) update_prefix=0; shift ;;
    *) fail "unknown argument $1 (--help)" ;;
    esac
done

[ -x "$host/loader/wine" ] && [ -x "$host/server/wineserver" ] ||
    fail "no complete host Wine build at $host (tools/build_host_wine.sh)"
command -v rsync >/dev/null 2>&1 || fail "rsync is missing"

mkdir -p "$drv/build"
echo "refresh_wine_host_drv: copying $host"
rsync -a --delete "$host/" "$drv/build/"

flags="$(sed -n 's/^CFLAGS = //p' "$drv/build/Makefile") -DWINE_PS5_USER_DRIVER -DWINE_PS5_HEADLESS_SURFACE"
rm -f "$drv/build"/dlls/win32u/*.o "$drv/build/dlls/win32u/win32u.so"
echo "refresh_wine_host_drv: win32u with the user driver (log: $drv/win32u-rebuild.log)"
(cd "$drv/build" && make -j"$jobs" dlls/win32u/win32u.so CFLAGS="$flags") > "$drv/win32u-rebuild.log" 2>&1 ||
    fail "win32u rebuild failed; see $drv/win32u-rebuild.log"
nm "$drv/build/dlls/win32u/ps5drv.o" | grep -q ' T ps5_install_user_driver$' ||
    fail "win32u was rebuilt without the user driver"

if [ "$update_prefix" = 1 ] && [ -f "$drv/prefix/system.reg" ]; then
    echo "refresh_wine_host_drv: updating $drv/prefix with $host/wine"
    WINEPREFIX="$drv/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="winemenubuilder.exe=d" \
        timeout 600 "$host/wine" wineboot -u >/dev/null 2>&1 || fail "wineboot -u failed on $drv/prefix"
    WINEPREFIX="$drv/prefix" "$host/server/wineserver" -w || true
fi
echo "refresh_wine_host_drv: $drv/build ready"
