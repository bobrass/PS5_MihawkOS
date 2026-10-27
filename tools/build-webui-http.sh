#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The WebUI's HTTP server library: GNU libmicrohttpd 1.0.10, pinned by SHA-256, built static without TLS (the WebUI
# serves the console's own network only). The recipe is PS5_RetroArch's tools/build-webui-http.sh, whose title links
# the same library with the same payload SDK. Prints the archive's path.
#
#   tools/build-webui-http.sh [ps5|host]     ps5: for the title (the payload SDK's prospero-clang); host: for host tests
#
# The tarball comes from PS5_RetroArch's cache beside this repository when it is there (read only, checked by hash),
# else from ftp.gnu.org; either way the hash below decides.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:-ps5}
[[ $mode == ps5 || $mode == host ]] || exit 2
version=1.0.10
hash=04bfe8ef75db7d629a33de767599765cecadc56274a39822d5d081030d577685
cache="$root/.deps/webui"
source_dir="$cache/libmicrohttpd-$version"
build="$root/build/webui-mhd-$mode"
archive="$cache/libmicrohttpd-$version.tar.gz"
mkdir -p "$cache" "$build"
if [[ ! -f $archive ]]; then
    sibling="$root/../PS5_RetroArch/.deps/webui/libmicrohttpd-$version.tar.gz"
    if [[ -f $sibling ]]; then
        cp "$sibling" "$archive.download"
    else
        curl -fL "https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-$version.tar.gz" -o "$archive.download"
    fi
    printf '%s  %s\n' "$hash" "$archive.download" | sha256sum -c - >&2
    mv "$archive.download" "$archive"
fi
printf '%s  %s\n' "$hash" "$archive" | sha256sum -c - >/dev/null
[[ -d $source_dir ]] || tar -xzf "$archive" -C "$cache"
stamp="$hash $mode $(sha256sum "$0" | cut -d' ' -f1)"
if [[ $mode == ps5 ]]; then
    sdk=$(bash "$root/ps5/tools/setup-sdk.sh")
    stamp+=" $(cat "$sdk/.ps5-sdk-revision")"
    export CC="$sdk/bin/prospero-clang" AR="$sdk/bin/prospero-ar" RANLIB="$sdk/bin/prospero-ranlib"
    export CFLAGS='-O1 -fPIC'
    # Cross-configure assumes a full FreeBSD libc; the title SDK lacks these symbols (PS5_RetroArch's finding):
    # MHD's bundled tree implementation and ordinary pipe wakeups instead.
    export mhd_cv_sys_tsearch_usable=no mhd_cv_works_func_pipe2=no
    host=(--host=x86_64-pc-freebsd)
else
    host=()
fi
if [[ ! -f $build/.stamp || $(cat "$build/.stamp") != "$stamp" ]]; then
    (
        cd "$build"
        if [[ -f Makefile ]]; then make distclean > clean.log 2>&1; fi
        "$source_dir/configure" "${host[@]}" --disable-shared --enable-static --disable-https \
            --disable-curl --disable-examples --disable-doc --disable-tools > configure.log 2>&1
        make -j"${BUILD_JOBS:-8}" > build.log 2>&1
    ) || { tail -30 "$build/configure.log" "$build/build.log" >&2; exit 1; }
    printf '%s\n' "$stamp" > "$build/.stamp"
fi
printf '%s\n' "$build/src/microhttpd/.libs/libmicrohttpd.a"
