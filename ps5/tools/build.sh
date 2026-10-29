#!/usr/bin/env bash
# PS5 Vulkan Template - build the title into dist/<TITLE_ID>/.
#
#   ps5/tools/build.sh
#
# Configures ps5/ with the payload SDK's CMake toolchain (once), compiles what
# changed, links with RADV and packages the title folder (tools/link-title.sh),
# then lays the assets in (tools/build-assets.py).
#
#   PS5_VULKAN_DIR   the PS5_Vulkan checkout (default ../PS5_Vulkan): the RADV
#                    release archive, the link recipe, the native tool and libc.prx
#
# The payload SDK is this title's own pin of my fork (tools/setup-sdk.sh).
#   PS5_CLANG        the host clang the compiler wrappers drive (default: clang)
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: MIT

set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
vulkan=$(cd -- "${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}" && pwd)
sdk=$(bash "$ps5/tools/setup-sdk.sh")
build="$root/build/ps5"
ninja=$(command -v ninja || echo "$HOME/.local/bin/ninja")
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}

if [[ ! -f $build/build.ninja ]]; then
    mkdir -p "$build"
    cmake -S "$ps5" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" \
        -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
        -DPS5_VULKAN_DIR="$vulkan" -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF > "$build.configure.log" 2>&1 \
        || { cat "$build.configure.log" >&2; exit 1; }
fi
"$ninja" -C "$build" title
# The title's link depends on the SDK's platform layer and RADV's archive (link-title.sh's depfile): a Ninja that does
# not know them keeps an old title after an SDK patch, which only deleting build/ps5/eboot.elf undid.
# (the listing is read whole before it is searched: piped into grep -q under pipefail, a SIGPIPE would fail it)
deps=$("$ninja" -C "$build" -t deps eboot.elf)
for library in libps5platform.a libvulkan_radeon.ps5.a; do
    grep -q "/$library\$" <<<"$deps" ||
        { echo "build.sh: Ninja does not know that eboot.elf depends on $library" >&2; exit 1; }
done
python3 "$ps5/tools/build-assets.py" --install "$root/dist/$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$ps5/sce_sys/param.json")"
