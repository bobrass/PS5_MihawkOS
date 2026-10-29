#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
# Every Windows link takes /timestamp:0, as the theme DLLs do: the same source links to the same bytes, so a
# rebuild changes nothing it should not (docs/BUILD_SPEED.md).
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
wine=$root/.deps/wine-runtime
output=${1:-$root/.deps/probes/graphics-render-probe.exe}
mkdir -p "$(dirname -- "$output")"
bash "$root/tools/build-process-probe.sh" "$(dirname -- "$output")/process-probe.exe"
clang --target=x86_64-windows --no-default-config -fuse-ld=lld -Wl,/timestamp:0 -nostdlib -O2 \
    -DPS5_GRAPHICS_CHILD_PROBE \
    -I"$wine/source/include" -I"$wine/build/include" -I"$wine/source/include/msvcrt" \
    -Wl,/entry:mainCRTStartup -Wl,/subsystem:console \
    "$root/tests/d3d11-probe.c" \
    "$wine/build/dlls/kernel32/x86_64-windows/libkernel32.a" \
    "$wine/build/dlls/user32/x86_64-windows/libuser32.a" \
    "$wine/build/dlls/d3d11/x86_64-windows/libd3d11.a" \
    "$wine/build/dlls/xinput1_3/x86_64-windows/libxinput.a" \
    "$wine/build/dlls/msvcrt/x86_64-windows/libmsvcrt.a" \
    -o "$output"
