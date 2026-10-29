#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Mesa's WGL over Zink for 32-bit Windows programs (docs/IA32_DESIGN.md, M4 step 3): opengl32.dll and
# libgallium_wgl.dll for i686, as the graphics foundation's tools/build-mesa-windows-zink.sh builds them for x86_64
# (tools/build-mesa-windows-zink.sh here), from the same pinned Mesa revision and compiler archive. The foundation's
# script builds one architecture and is pinned with it, so this one is its i686 counterpart; they differ in the
# cross file and the output, nothing else. No fetches.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=$root/.deps/mesa-foundation-wgl-i686
mesa_fork=$(cd -- "$root/../PS5_Mesa" && pwd)
mingw_archive=$(realpath "$root/.deps/llvm-mingw.tar.xz")
revision=a8800d9130583fbd4a436c4c64ab9e33b98edf10
archive_sha256=bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21
printf '%s  %s\n' "$archive_sha256" "$mingw_archive" | sha256sum --check --status
mkdir -p "$work"
mingw=$work/toolchain
if [ ! -f "$mingw/.archive-sha256" ] || [ "$(cat "$mingw/.archive-sha256")" != "$archive_sha256" ]; then
    mkdir -p "$mingw"
    tar -xJf "$mingw_archive" -C "$mingw" --strip-components=1
    printf '%s\n' "$archive_sha256" > "$mingw/.archive-sha256"
fi
if [ ! -f "$work/source/.revision" ] || [ "$(cat "$work/source/.revision")" != "$revision" ]; then
    staging=$(mktemp -d "$work/source.XXXXXX")
    git -C "$mesa_fork" archive "$revision" | tar -x -C "$staging"
    printf '%s\n' "$revision" > "$staging/.revision"
    rm -rf "$work/source"
    mv "$staging" "$work/source"
fi
[ "$(cat "$work/source/VERSION")" = 26.2.0 ]
cat > "$work/cross.ini" <<CROSS
[binaries]
c = '$mingw/bin/i686-w64-mingw32-clang'
cpp = '$mingw/bin/i686-w64-mingw32-clang++'
ar = '$mingw/bin/llvm-ar'
strip = '$mingw/bin/llvm-strip'
windres = '$mingw/bin/llvm-windres'
pkg-config = 'false'
[host_machine]
system = 'windows'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
[properties]
needs_exe_wrapper = true
CROSS
export PATH="$mingw/bin:$PATH"
# No link time in the DLLs (--no-insert-timestamp), as the x86_64 build (foundation patch 0050): the same source gives
# the same bytes.
reconfigure=()
[ ! -f "$work/build/build.ninja" ] || reconfigure=(--reconfigure)
meson setup "${reconfigure[@]}" "$work/build" "$work/source" --cross-file "$work/cross.ini" \
    --wrap-mode=nofallback --buildtype=release --default-library=static \
    -Dplatforms=windows -Dgallium-drivers=zink -Dvulkan-drivers= -Dopengl=true \
    -Degl=disabled -Dgbm=disabled -Dglx=disabled -Dgles1=disabled -Dgles2=disabled \
    -Dllvm=disabled -Dzlib=disabled -Dzstd=disabled -Dshader-cache=disabled \
    -Dxmlconfig=disabled -Dexpat=disabled -Dlibunwind=disabled -Dvalgrind=disabled \
    -Dvideo-codecs= -Dbuild-tests=false -Dtools= -Dgallium-va=disabled -Dgallium-rusticl=false \
    -Dc_link_args=-Wl,--no-insert-timestamp -Dcpp_link_args=-Wl,--no-insert-timestamp \
    > "$work/meson.log" 2>&1 || { tail -30 "$work/meson.log" >&2; exit 1; }
ninja -C "$work/build" -j "${JOBS:-12}" \
    src/gallium/targets/libgl-gdi/opengl32.dll src/gallium/targets/wgl/libgallium_wgl.dll \
    > "$work/ninja.log" 2>&1 || { tail -30 "$work/ninja.log" >&2; exit 1; }
python3 - "$root" "$work" "$revision" <<'PY'
import hashlib,json,sys
from pathlib import Path
root,work,revision=Path(sys.argv[1]),Path(sys.argv[2]),sys.argv[3]
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
files={p.name:digest(p) for p in work.glob('build/src/gallium/targets/*/*.dll')}
manifest={'mesa_version':'26.2.0','mesa_revision':revision,'llvm_mingw':'20260922-ucrt',
          'build_script':digest(root/'tools/build-mesa-windows-zink-i686.sh'),'architecture':'i686',
          'driver':'zink','files':files,'console_validated':False}
(work/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Built i686 Mesa WGL/Zink:',', '.join(files))
PY
