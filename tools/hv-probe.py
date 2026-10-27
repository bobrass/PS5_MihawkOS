#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Kernel memory accounting and core-0 VMCB fields, read-only: tools/hv-probe.c, built with the payload SDK and sent
through websrv's launcher (POST /elfldr, pipe=1), which streams its output back.

  tools/hv-probe.py

Needs the kernel exploit backend on the console (kstuff) for kernel_getlong/getint, and Byepervisor Reborn already
run for NESTED_CTRL to show nested paging off. The build uses PS5_HV_SDK if set, else the repository's pinned SDK.
"""
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

SOURCE = ROOT / 'tools/hv-probe.c'
OUTPUT = ROOT / 'build/hv-probe.elf'
SDK = Path(os.environ.get('PS5_HV_SDK') or ROOT / '.deps/native/ps5-payload-sdk')


def main():
    if sys.argv[1:]:
        raise SystemExit(__doc__)
    if not OUTPUT.exists() or OUTPUT.stat().st_mtime < SOURCE.stat().st_mtime:
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(SDK / 'bin/prospero-clang'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-o', str(OUTPUT), str(SOURCE)], check=True)
    settings = ps5_console.load_settings()
    boundary = 'HvProbe'
    body = b''
    for name, data in (('args', b'hv-probe'), ('pipe', b'1'), ('elf', OUTPUT.read_bytes())):
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="hv-probe.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            print(response.read().decode(errors='replace').replace(settings['host'], '<console>'), end='')
    except OSError as error:
        raise SystemExit(f'websrv: {str(error).replace(settings["host"], "<console>")}') from None


if __name__ == '__main__':
    main()
