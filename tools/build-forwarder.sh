#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The game forwarder's programs (forwarder/): the app every home screen tile runs (eboot.bin, linked as the
# title's is, with PS5_Vulkan's CRT, linker script and native tool, and nothing of MihawkOS in it) and the launch
# payload a tile sends to the console's ELF loader (launch.elf, built with the payload SDK), and the tiles' worker the
# WebUI sends there when a tile is made or removed (worker.elf, forwarder/worker.c). The WebUI copies the first two into
# each tile it makes (src/pw_webui.hpp); the production build puts them in the title folder's forwarder/. PS5_RetroArch's
# tools/build-forwarder.sh, on this repository's pins. Prints the folder holding them.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
vulkan=$(cd -- "${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}" && pwd)
sdk=$(bash "$root/ps5/tools/setup-sdk.sh")
native="$vulkan/tooling/native"
tool="$vulkan/build/host/ps5-native-tool"
out="$root/build/forwarder"
[[ -x $sdk/bin/prospero-clang ]] || { echo "error: no SDK at $sdk" >&2; exit 2; }
[[ -x $tool ]] || { echo "error: no $tool; build PS5_Vulkan's native tool" >&2; exit 2; }
target=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/ps5/sce_sys/param.json")
[[ $target =~ ^[A-Z]{4}[0-9]{5}$ ]] || { echo "error: no title id in ps5/sce_sys/param.json" >&2; exit 2; }
rm -rf -- "$out"
mkdir -p "$out/obj"

# The payload SDK's driver links with --build-id=uuid, a new id each build: the same source gave launch.elf and
# worker.elf of the same size with other bytes, which the title deploy (by size) skipped (c4a3458, 2026-10-27). md5 is
# the output's own hash, so one source gives one file.
# The launch payload: what it starts is this title.
PS5_PAYLOAD_SDK="$sdk" PS5_CLANG=${PS5_CLANG:-clang} "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror \
    -Wl,--build-id=md5 -DFORWARDER_TARGET="\"$target\"" -o "$out/launch.elf" "$root/forwarder/launch.c" \
    -lSceSystemService -lSceUserService
grep -q "MIHAWK-FORWARDER-TILE-ID-SLOT" "$out/launch.elf" ||
    { echo "error: launch.elf has no slot for a tile's title id" >&2; exit 1; }
# The tiles' worker: installs and removals wait for MihawkOS to close (forwarder/worker.c), in a payload of its own.
PS5_PAYLOAD_SDK="$sdk" PS5_CLANG=${PS5_CLANG:-clang} "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror \
    -Wl,--build-id=md5 -o "$out/worker.elf" "$root/forwarder/worker.c" -lSceSystemService

# The tile's app. PS5_Vulkan's CRT sets the FPU through the SDK's platform layer (ps5_fp_ieee).
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }
cc -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -c "$root/forwarder/tile.c" -o "$out/obj/tile.o"
for part in app_crt app_cpp_runtime; do
    cc -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
        -c "$native/$part.cpp" -o "$out/obj/$part.o"
done
"$sdk/bin/prospero-lld" -T "$native/ps5-pie.ld" -L "$native" --eh-frame-hdr --error-limit=0 \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL -e _start -o "$out/tile.elf" \
    "$out/obj/app_crt.o" "$out/obj/app_cpp_runtime.o" "$out/obj/tile.o" "$sdk/target/lib/libps5platform.a" \
    --as-needed "$sdk"/target/lib/*.so
# An import only libkernel_sys or libScePosixForWebKit defines is null at run time in a title (PS5_RetroArch's tile
# called sceLncUtilLaunchApp, null for a title, and faulted at 0): refused here.
mapfile -t null_imports < <(comm -23 \
    <("$sdk/bin/llvm-nm" -D --undefined-only "$out/tile.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for stub in "$sdk"/target/lib/*.so; do
        case $(basename "$stub") in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$sdk/bin/llvm-nm" -D --defined-only "$stub" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
((${#null_imports[@]} == 0)) || { echo "error: the tile imports what no module exports: ${null_imports[*]}" >&2; exit 1; }
"$tool" link --in "$out/tile.elf" --out "$out/tile.module.elf" --stub-dir "$sdk/target/lib" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf >&2
"$tool" self --sign --in "$out/tile.module.elf" --out "$out/eboot.bin" --magic 0x1D3D154F >&2
printf '==> [forwarder] tile app %s bytes, launch payload %s bytes (starts %s), tile worker %s bytes\n' \
    "$(stat -c %s "$out/eboot.bin")" "$(stat -c %s "$out/launch.elf")" "$target" "$(stat -c %s "$out/worker.elf")" >&2
printf '%s\n' "$out"
