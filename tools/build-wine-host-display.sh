#!/usr/bin/env bash
# The conformance oracle for programs with windows: the pinned runtime's own Wine source and revision
# (.deps/wine-runtime/source, tools/build-wine-runtime.sh), built for this PC with its X11 driver. The pinned build is
# headless (--without-x), so on it user32, gdi32, input and Direct3D tests time out or skip, and Inno's and Burn's
# silent installers cannot make their windows. Only --with-x differs; nothing is installed.
#
#   tools/build-wine-host-display.sh          then: tools/run-wine-tests.py host DLL --host-wine .deps/wine-host-display/build/wine
#
# Runs on the user's display, so tools/run-wine-tests.py puts every window of it into one Wine virtual desktop.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source=$root/.deps/wine-runtime/source
work=$root/.deps/wine-host-display
revision=$(cat "$root/.deps/wine-runtime/revision")
configure_args=(--enable-win64 --enable-archs=i386,x86_64 --with-mingw=clang --disable-tests
    --with-x --without-wayland --without-gstreamer --without-ffmpeg --without-alsa --without-pulse)
stamp=$(printf '%s\n' "$revision" "${configure_args[@]}" | sha256sum | cut -c1-64)
failed() { tail -n 20 "$1" >&2; echo "tools/build-wine-host-display.sh failed: the whole log is $1" >&2; exit 1; }
[[ -d $source ]] || { echo "build the runtime first: tools/build-wine-runtime.sh" >&2; exit 1; }
mkdir -p "$work/build"
if [[ $(cat "$work/stamp" 2>/dev/null) != "$stamp" ]]; then
    (cd "$work/build" && "$source/configure" "${configure_args[@]}" > "$work/configure.log" 2>&1) || failed "$work/configure.log"
    grep -q 'winex11' "$work/build/Makefile" || { echo "configure found no X11: winex11.drv would not be built" >&2; exit 1; }
    echo "$revision" > "$work/revision"
    echo "$stamp" > "$work/stamp"
fi
make -C "$work/build" -j"${JOBS:-12}" > "$work/make.log" 2>&1 || failed "$work/make.log"
echo "display-capable Wine of revision $revision built in $work/build"
