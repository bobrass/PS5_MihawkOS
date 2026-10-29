#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The system's page tables over time (tools/console-pagetables.c), built with the payload SDK and sent through websrv's
launcher (POST /elfldr, pipe=1), which streams its output back. It only reads. The lines go to klog/NAME-pagetables.txt
as they come; run tools/klog-stream.py beside it to place them against the kernel log's events.

  tools/console-pagetables.py --name NAME [--seconds S] [--build-only]
"""
import argparse
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

SOURCE = ROOT / 'tools/console-pagetables.c'
OUTPUT = ROOT / 'build/console-pagetables.elf'
SDK = ROOT / '.deps/native/ps5-payload-sdk'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--seconds', type=int, default=1200)
    parser.add_argument('--build-only', action='store_true', help='build the payload and send nothing')
    args = parser.parse_args()
    if not args.name.replace('-', '').replace('_', '').isalnum() or not 0 < args.seconds <= 3600:
        parser.error('use a plain name and 1-3600 seconds')
    if not OUTPUT.exists() or OUTPUT.stat().st_mtime < SOURCE.stat().st_mtime:
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(SDK / 'bin/prospero-clang'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-o', str(OUTPUT), str(SOURCE)], check=True)
    if args.build_only:
        return
    settings = ps5_console.load_settings()
    boundary = 'ConsolePageTables'
    body = b''
    for name, data in (('args', f'console-pagetables {args.seconds}'.encode()), ('pipe', b'1'),
                       ('elf', OUTPUT.read_bytes())):
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="console-pagetables.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    path = ROOT / 'klog' / f'{args.name}-pagetables.txt'
    try:
        with urllib.request.urlopen(request, timeout=60) as response, path.open('a') as out:
            for line in response:
                out.write(line.decode(errors='replace').replace(settings['host'], '<console>'))
                out.flush()
    except OSError as error:
        raise SystemExit(f'websrv: {str(error).replace(settings["host"], "<console>")}') from None
    print(f'pagetables: {path}')


if __name__ == '__main__':
    main()
