#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Page-table fill probe: tools/fdstress.c, built with the payload SDK and sent through websrv's launcher
(POST /elfldr, pipe=1), which streams its output back.

  tools/fdstress.py

The host comes from PS5_HOST, never inferred. Needs the kernel exploit backend only for the kernel R/W probes; the
mmap loop itself is an ordinary payload.
"""
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

SOURCE = ROOT / 'tools/fdstress.c'
OUTPUT = ROOT / 'build/fdstress.elf'
SDK = Path(os.environ.get('PS5_HV_SDK') or ROOT / '.deps/native/ps5-payload-sdk')


def main():
    if not OUTPUT.exists() or OUTPUT.stat().st_mtime < SOURCE.stat().st_mtime:
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(SDK / 'bin/prospero-clang'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-o', str(OUTPUT), str(SOURCE)], check=True)
    settings = ps5_console.load_settings()
    boundary = 'PtFill'
    body = b''
    for name, data in (('args', b'fdstress ' + ' '.join(sys.argv[1:]).encode()), ('pipe', b'1'), ('elf', OUTPUT.read_bytes())):
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="fdstress.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    try:
        with urllib.request.urlopen(request, timeout=180) as response:
            print(response.read().decode(errors='replace').replace(settings['host'], '<console>'), end='')
    except OSError as error:
        raise SystemExit(f'websrv: {str(error).replace(settings["host"], "<console>")}') from None


if __name__ == '__main__':
    main()
