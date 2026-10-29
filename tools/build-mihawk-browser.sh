#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# MihawkBrowser (docs/MIHAWK_BROWSER.md §3.6): src/pw_browser.c, pw_browser_chrome.c and pw_browser_policy.c, built with clang for
# x86_64-windows against CEF's C API at the pinned API version (CEF_API_VERSION in pw_browser.c), linked with CEF's
# libcef.lib and Wine's import libraries, as tools/build-wine-probe.sh builds mihawk-appearance.exe. The result is
# .deps/mihawk-browser/app: MihawkBrowser.exe beside the ship list of tools/fetch-cef.py, the folder the runtime
# carries once, read-only, outside every prefix. Fails when a shipped file is missing. x64 only.
set -euo pipefail
# Every Windows link takes /timestamp:0, as the theme DLLs do: the same source links to the same bytes, so a
# rebuild changes nothing it should not (docs/BUILD_SPEED.md).
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
wine=$root/.deps/wine-runtime
python3 "$root/tools/fetch-cef.py" > /dev/null
cef=$root/.deps/cef/$(sed -n "s/^NAME = '\(.*\)'$/\1/p" "$root/tools/fetch-cef.py")
out=$root/.deps/mihawk-browser
app=$out/app
rm -rf "$app"
mkdir -p "$app/locales"
printf '1 24 "%s/src/pw_browser.manifest"\n' "$root" > "$out/browser.rc"
"$root/.deps/llvm-mingw/bin/x86_64-w64-mingw32-windres" "$out/browser.rc" "$out/browser.o"
libs=()
for dll in kernel32 user32 gdi32 advapi32 shell32 ole32 comctl32 uxtheme dwmapi bcrypt msvcrt; do
    libs+=("$wine/build/dlls/$dll/x86_64-windows/lib$dll.a")
done
clang --target=x86_64-windows --no-default-config -fuse-ld=lld -Wl,/timestamp:0 -nostdlib -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -D_MSVCR_VER=0 \
    -isystem "$wine/source/include" -isystem "$wine/build/include" -isystem "$wine/source/include/msvcrt" -isystem "$cef" \
    -Wl,/entry:mainCRTStartup -Wl,/subsystem:windows \
    "$root/src/pw_browser.c" "$root/src/pw_browser_chrome.c" "$root/src/pw_browser_policy.c" "$out/browser.o" "$cef/Release/libcef.lib" "${libs[@]}" \
    -o "$app/MihawkBrowser.exe"
python3 - "$cef/ship.json" "$cef" "$app" <<'EOF'
import json, os, sys
ship, cef, app = json.load(open(sys.argv[1])), sys.argv[2], sys.argv[3]
for name in ship['ship']:
    target = os.path.join(app, 'locales' if '/locales/' in name else '', os.path.basename(name))
    os.link(os.path.join(cef, name), target)  # hard links: the ship list is 316 MB
EOF
python3 "$root/tools/audit-pe.py" "$app/MihawkBrowser.exe"
