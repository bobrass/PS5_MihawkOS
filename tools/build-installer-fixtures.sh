#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the installer slice's fixtures into .deps/installers/fixtures/ (tests/installer-slice-probe.c runs them):
#
#   installer-slice-probe.exe, steam-app/ (pwapp32.exe, pwapp64.exe, pwapp.dat, installscript.vdf)
#   fixture-nsis.exe   makensis (nsis), from tests/installers/fixture.nsi
#   fixture.msi        wixl (msitools), from tests/installers/fixture.wxs
#   fixture-inno.exe   Inno Setup's ISCC.exe under the PC's own Wine, from tests/installers/fixture.iss
#   the Microsoft redistributables, copied from tools/fetch-installers.py's pinned downloads
#
# Idempotent; a fixture whose tool is missing is reported and left out (the slice then reports that kind missing).
#   tools/build-installer-fixtures.sh
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
wine=$root/.deps/wine-runtime
out=$root/.deps/installers/fixtures
downloads=$root/.deps/installers/downloads
src=$root/tests/installers
mkdir -p "$out/steam-app"
python3 "$root/tools/fetch-installers.py"
flags=(--no-default-config -fuse-ld=lld -nostdlib -O2 -Wall -Wextra -Werror -I"$wine/source/include" -I"$wine/build/include"
       -I"$wine/source/include/msvcrt" -Wl,/entry:mainCRTStartup -Wl,/subsystem:console)
clang --target=x86_64-windows "${flags[@]}" -Wno-missing-field-initializers "$root/tests/installer-slice-probe.c" \
    "$wine/build/dlls/kernel32/x86_64-windows/libkernel32.a" "$wine/build/dlls/advapi32/x86_64-windows/libadvapi32.a" \
    "$wine/build/dlls/msvcrt/x86_64-windows/libmsvcrt.a" -o "$out/installer-slice-probe.exe"
clang --target=x86_64-windows "${flags[@]}" "$src/pwapp.c" "$wine/build/dlls/kernel32/x86_64-windows/libkernel32.a" -o "$out/steam-app/pwapp64.exe"
clang --target=i686-windows "${flags[@]}" "$src/pwapp.c" "$wine/build/dlls/kernel32/i386-windows/libkernel32.a" -o "$out/steam-app/pwapp32.exe"
cp "$src/pwapp.dat" "$src/steam-app/installscript.vdf" "$out/steam-app/"
for name in vc_redist.x64.exe vc_redist.x86.exe directx_Jun2010_redist.exe; do
    cmp -s "$downloads/$name" "$out/$name" || cp "$downloads/$name" "$out/$name"
done

if command -v makensis > /dev/null; then
    makensis -V2 -DAPPDIR="$out/steam-app" -DOUT="$out/fixture-nsis.exe" "$src/fixture.nsi"
else
    # NSIS's own Windows build (pinned zip) on the PC's Wine, in the compiler prefix below
    nsis=$root/.deps/installers/nsis
    [[ -f $nsis/nsis-3.13/makensis.exe ]] || { rm -rf "$nsis"; mkdir -p "$nsis"; (cd "$nsis" && 7z x -y "$downloads/nsis-3.13.zip" > /dev/null); }
    WINEPREFIX=$root/.deps/installers/inno-prefix WINEDEBUG=-all wine "$nsis/nsis-3.13/makensis.exe" -V2 \
        "-DAPPDIR=Z:${out//\//\\}\\steam-app" "-DOUT=Z:${out//\//\\}\\fixture-nsis.exe" "Z:${src//\//\\}\\fixture.nsi"
    wineserver -k 2> /dev/null || true
fi
if command -v wixl > /dev/null; then
    wixl -a x64 -D APPDIR="$out/steam-app" -o "$out/fixture.msi" "$src/fixture.wxs"
else
    echo "msi: wixl is not installed (sudo pacman -S msitools); fixture.msi left out" >&2
fi

# Inno Setup's compiler, installed once into a prefix of its own (never a test prefix). It needs windows even when
# silent, which the pinned headless Wine cannot make, so this build tool runs on the PC's own Wine (wine on PATH).
inno_prefix=$root/.deps/installers/inno-prefix
iscc=$inno_prefix/drive_c/inno/ISCC.exe
export WINEPREFIX=$inno_prefix WINEDEBUG=-all
if [[ ! -f $iscc ]]; then
    wine "$downloads/innosetup-7.1.0-x64.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /CURRENTUSER \
        '/DIR=C:\inno' /NOICONS > /dev/null 2>&1 || true
    [[ -f $iscc ]] || { echo "inno: ISCC.exe not installed into $inno_prefix" >&2; exit 1; }
fi
work=$inno_prefix/drive_c/fixture
rm -rf "$work"; mkdir -p "$work"
cp "$src/fixture.iss" "$out/steam-app/"pwapp* "$work/"
wine "$iscc" /Q '/DAPPDIR=C:\fixture' '/OC:\fixture' 'C:\fixture\fixture.iss' > "$work/iscc.log" 2>&1 ||
    { cat "$work/iscc.log" >&2; exit 1; }
cp "$work/fixture-inno.exe" "$out/fixture-inno.exe"
wineserver -k 2> /dev/null || true

( cd "$out" && sha256sum installer-slice-probe.exe steam-app/* fixture-* fixture.msi 2> /dev/null ) > "$out/manifest.sha256" || true
ls -la "$out"
