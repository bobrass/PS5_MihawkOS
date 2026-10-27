#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Kernel/HV read-write payload: tools/hvctl.c, built with the payload SDK and sent through websrv's launcher
(POST /elfldr, pipe=1), which streams its output back.

  tools/hvctl.py pmap
  tools/hvctl.py read=0xADDR
  tools/hvctl.py dump=0xADDR len=0x100
  tools/hvctl.py write=0xADDR value=0xV      (explicit; writes and reads back)

Needs the kernel exploit backend (kstuff) on the console. The build uses PS5_HV_SDK if set, else the repository's
pinned SDK. The host comes from PS5_HOST, never inferred here.
"""
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

SOURCE = ROOT / 'tools/hvctl.c'
OUTPUT = ROOT / 'build/hvctl.elf'
SDK = Path(os.environ.get('PS5_HV_SDK') or ROOT / '.deps/native/ps5-payload-sdk')


def main():
    words = sys.argv[1:] or ['pmap']
    if not OUTPUT.exists() or OUTPUT.stat().st_mtime < SOURCE.stat().st_mtime:
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(SDK / 'bin/prospero-clang'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-o', str(OUTPUT), str(SOURCE)], check=True)
    settings = ps5_console.load_settings()
    boundary = 'HvCtl'
    detach = 'detach' in words
    words = [w for w in words if w != 'detach']
    args = ' '.join(['hvctl'] + words).encode()
    body = b''
    for name, data in (('args', args), ('pipe', b'0' if detach else b'1'), ('elf', OUTPUT.read_bytes())):
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="hvctl.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    try:
        if detach:
            # pipe=0: the loader starts the payload and answers without streaming its output, so the connection does
            # not sit on the endpoint the runtime spawns helpers through.
            with urllib.request.urlopen(request, timeout=30) as response:
                response.read(256)
            return
        with urllib.request.urlopen(request, timeout=600) as response:
            while True:
                chunk = response.read(4096)
                if not chunk:
                    break
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
    except OSError as error:
        raise SystemExit(f'websrv: {str(error).replace(settings["host"], "<console>")}') from None


if __name__ == '__main__':
    main()
