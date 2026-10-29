#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify independent HWND pixels and input on the actual composed display."""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def display_verdict(captures, log, graphics=False):
    sample_counts = (4, 4, 3, 4, 3, 3, 4) if graphics else (4, 4, 3, 4, 3, 4, 3, 3, 3, 3, 3, 3, 3, 3)
    # The GPU probe's seventh capture is a window larger than the screen (graphics patch 0036): no desktop shows.
    covered = {7} if graphics else set()
    samples = re.findall(r'window-compose capture=(\d+) phase=([\w-]+) sample=(\d+),(\d+) '
                         r'expected=(\d+),(\d+),(\d+) result=(PASS|FAIL)', log)
    results = []
    wallpaper = ROOT / '.deps/staged/runtime/ge11-7/lib/wine/prefix/drive_c/windows/web/wallpaper/proton.bmp'
    expected_background = None
    if wallpaper.is_file():
        with Image.open(wallpaper) as source:
            expected_background = source.convert('RGB').getpixel((1600, 900))
    else:
        colors=set(re.findall(r'window-compose desktop_colour=(\d+),(\d+),(\d+)',log))
        if len(colors)==1:
            expected_background=tuple(int(component) for component in colors.pop())
    for number, capture in enumerate(captures, 1):
        unique = set(tuple(s) for s in samples if int(s[0]) == number)
        with Image.open(ROOT / capture) as source:
            image = source.convert('RGB')
            pixels = []
            for _, phase, x, y, r, g, b, verdict in sorted(unique):
                point = (int(x), int(y))
                expected = (int(r), int(g), int(b))
                actual = image.getpixel(point) if image.size == (1920, 1080) else None
                pixels.append({'phase': phase, 'point': point, 'expected': expected, 'actual': actual,
                               'passed': verdict == 'PASS' and actual == expected})
            background = image.getpixel((1600, 900)) if image.size == (1920, 1080) else None
            results.append({'capture': capture, 'number': number, 'pixels': pixels,
                            'desktop_background': background, 'expected_background': expected_background,
                            'passed': (number in covered or expected_background is not None and background == expected_background) and number <= len(sample_counts) and len(pixels) == sample_counts[number - 1] and all(p['passed'] for p in pixels)})
    return results, len(results) == len(sample_counts) and all(r['passed'] for r in results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--arch', choices=('32', '64'), required=True)
    parser.add_argument('--graphics', action='store_true', help='qualify nested OpenGL/Zink HWNDs')
    parser.add_argument('--backend', choices=('gl', 'dxvk'), default='gl')
    parser.add_argument('--env', action='append', default=[], help='NAME=VALUE for this launch only')
    parser.add_argument('--dmem', action='store_true', help='require the broker to have taken every host-visible buffer from direct memory and given it all back')
    parser.add_argument('--fallback', action='store_true', help='with --dmem: the helper refused some ranges (PROTON_GPU_TEST_DMEM_REFUSE), so some went to shared memory')
    parser.add_argument('--readback', action='store_true', help='diagnose DXVK pixels before Present; not used by the gate')
    args = parser.parse_args()
    if args.backend == 'dxvk' and not args.graphics:
        parser.error('DXVK requires --graphics')
    if args.readback and args.backend != 'dxvk':
        parser.error('readback requires DXVK')
    probe = 'gpu-window' if args.graphics else 'window-compose'
    marker = 'PS5_PROTON_GPU_WINDOW' if args.graphics else 'PS5_PROTON_WINDOW_COMPOSE'
    result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', args.name,
        '--path', r'C:\windows\system32\mihawk.exe', '--args', f'C:\\{probe}{args.arch}-probe.exe' + (' --dxvk' if args.backend == 'dxvk' else '') + (' --readback' if args.readback else ''),
        '--marker', marker, '--env', 'PROTON_LAYOUT=launcher',
        '--env', 'WINE_PS5_WINDOW_SURFACES=1', '--seconds', '180', '--watchdog', '220'] + [x for e in args.env for x in ('--env', e)], cwd=ROOT)
    path = ROOT / 'evidence' / (args.name + '.json')
    if not path.exists():
        return result.returncode or 1
    record = json.loads(path.read_text())
    log = '\n'.join(p.read_text(errors='replace') for p in (ROOT / 'klog').glob(args.name + '*.log'))
    pixels, passed = display_verdict(record.get('captures', []), log, args.graphics)
    record['semantics_passed'] = record['passed']
    record['window_pixels'] = pixels
    dmem_ok = True
    if args.dmem:
        import re
        lines = re.findall(r'gpu-broker dmem_host made=(\d+) freed=(\d+) refused=(\d+) free_failed=(\d+)', log)
        made, freed, refused, failed = map(int, lines[-1]) if lines else (0, 0, 1, 1)
        dmem_ok = made > 0 and freed == made and not failed and (refused > 0 if args.fallback else not refused)
        record['host_dmem'] = {'made': made, 'freed': freed, 'refused': refused, 'free_failed': failed, 'passed': dmem_ok}
    record['passed'] = result.returncode == 0 and record['passed'] and passed and dmem_ok
    path.write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps({'window_pixels': pixels, 'passed': record['passed']}, indent=1))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
