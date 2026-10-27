#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Export only the pinned Aurelia client binary, verified by sha256; never fetch it implicitly. The launcher runs it
# under Wine (the prefix maps Z: to the console's root, so Z:\data\ps5proton\aurelia\aurelia_windows_x86_64.exe works
# without copying it into C:), speaking its --json lines (src/pw_aurelia.h).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=0.1.40
sha256=477f3d7b70866c5103a0ae17f5a95fb753067d7e376704220db8b125a126a806
asset=aurelia_windows_x86_64.exe
url=https://github.com/Drackrath/Aurelia/releases/download/v$version/$asset
out=$root/.deps/aurelia
dest=$out/$asset
if [[ -f $dest && $(sha256sum "$dest" | cut -d' ' -f1) == "$sha256" ]]; then
    printf '%s\n' "$dest"
    exit 0
fi
mkdir -p "$out"
staging=$(mktemp "$out/.fetch.XXXXXX")
trap 'rm -f "$staging"' EXIT
curl -sSL --fail --retry 2 --max-time 300 -o "$staging" "$url"
got=$(sha256sum "$staging" | cut -d' ' -f1)
if [[ $got != "$sha256" ]]; then
    echo "fetch-aurelia: $url is sha256 $got, pinned $sha256" >&2
    exit 1
fi
mv "$staging" "$dest"
trap - EXIT
printf '%s\n' "$dest"
