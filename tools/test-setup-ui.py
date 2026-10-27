#!/usr/bin/env python3
"""The guided setup's pictures on the PC, and its checks (docs/GUIDED_SETUP.md).

    VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json python3 tools/test-setup-ui.py

After ps5/tools/build.sh and ps5/tools/host-reference.sh. Each step is shown through PROTON_SETUP_TEST
(<step>[:<the fake install's scenario>][@<share held>]) and PROTON_SETUP_LANGUAGE, walked with the test run's presses,
and kept as klog/ui-host/setup-<name>.png. The checks: every run ends well, the log names the step it reached, the
picture is not blank, the QR code reads back as the instructions' address, and choosing a language switches the text.

The Steam guide (src/pw_steam_guide.h): its launcher pages through PROTON_STEAM_GUIDE_TEST
(<page>[:<failure>][@<share downloaded>][#<display name>]) as setup-steam-<page>-<language>.png, every page in every
language and every failure in English; and the screens its sessions draw, from the same C code the title runs
(tests/test_pw_steam_screen.c's pictures), as setup-steam-session-<screen>-<language>.png.
Copyright (C) 2026 Mihawk
SPDX-License-Identifier: GPL-3.0-or-later
"""
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/ps5-host'
APP = BUILD / 'app'
OUT = ROOT / 'klog/ui-host'
URL = re.search(r'SETUP_INSTRUCTIONS = "([^"]+)"', (ROOT / 'ps5/tools/generate_assets.py').read_text()).group(1)

COVER_LANGUAGES = ('en-US', 'fr-FR', 'es-ES', 'de-DE', 'it-IT', 'ru-RU', 'zh-CN', 'ja-JP', 'pt-BR', 'pl-PL')
STEAM_PAGES = {'intro': 0, 'download@0.45': 1, 'starting': 1, 'failed:offline': 2, 'done#Mihawk': 3}  # PwSteamPage
STEAM_FAILURES = ('offline', 'clock', 'download', 'install', 'stalled', 'cancelled', 'timeout', 'guard', 'ended')
COVER_STAGES = ('settings', 'dotnet', 'finishing', 'ready')  # PwSetupBuild's order

# name, PROTON_SETUP_TEST, language, frames, presses (frame, action), the step the log must end on
RENDERS = [
    ('welcome', '0', 'en-US', 90, [], 0),
    ('language', '1', 'en-US', 60, [], 1),
    ('language-pick-french', '1', 'en-US', 90, [(20, 'right'), (28, 'cross')], 2),
    ('method', '2', 'en-US', 60, [], 2),
    ('usb', '3', 'en-US', 60, [], 3),
    ('usb-no-drive', '3:no-stick', 'en-US', 60, [(10, 'cross')], 3),
    ('usb-partial', '3:partial', 'en-US', 60, [(10, 'cross')], 3),
    ('usb-found', '3', 'en-US', 60, [(10, 'cross')], 3),
    ('installing', '4@0.62', 'en-US', 40, [], 4),
    ('installing-damaged', '4:damaged@0.5', 'en-US', 40, [], 4),
    ('preparing', '5', 'en-US', 60, [], 5),
    ('done', '6', 'en-US', 90, [], 6),
    ('welcome-fr', '0', 'fr-FR', 90, [], 0),
    ('usb-fr-no-drive', '3:no-stick', 'fr-FR', 60, [(10, 'cross')], 3),
    ('installing-fr', '4@0.62', 'fr-FR', 40, [], 4),
    ('preparing-fr', '5', 'fr-FR', 60, [], 5),
    ('method-de', '2', 'de-DE', 60, [], 2),
    ('installing-ru', '4@0.62', 'ru-RU', 40, [], 4),
    ('usb-zh-no-drive', '3:no-stick', 'zh-CN', 60, [(10, 'cross')], 3),
    ('welcome-zh', '0', 'zh-CN', 90, [], 0),
    ('preparing-ja', '5', 'ja-JP', 60, [], 5),
    ('language-ja', '1', 'ja-JP', 60, [], 1),
    ('done-es', '6', 'es-ES', 90, [], 6),
    ('usb-pt-partial', '3:partial', 'pt-BR', 60, [(10, 'cross')], 3),
    ('method-it', '2', 'it-IT', 60, [], 2),
    # Polish (pl-PL, the tenth): its pages, the ten language tiles, and its pick from them (the last tile)
    ('welcome-pl', '0', 'pl-PL', 90, [], 0),
    ('language-pl', '1', 'pl-PL', 60, [], 1),
    ('language-pick-polish', '1', 'en-US', 90, [(14, 'down'), (20, 'right'), (26, 'right'), (32, 'right'), (38, 'right'),
                                               (46, 'cross')], 2),
    ('method-pl', '2', 'pl-PL', 60, [], 2),
    ('usb-pl-no-drive', '3:no-stick', 'pl-PL', 60, [(10, 'cross')], 3),
    ('installing-pl', '4@0.62', 'pl-PL', 40, [], 4),
    ('preparing-pl', '5', 'pl-PL', 60, [], 5),
    ('done-pl', '6', 'pl-PL', 90, [], 6),
    ('ftp-pl', '7', 'pl-PL', 60, [], 7),
    # The FTP route (step 7, numbered after Done): its page, an empty folder, a partial one, the files found, and the
    # method tile chosen with the pad (right from USB, then cross)
    ('ftp', '7', 'en-US', 60, [], 7),
    ('ftp-empty', '7:no-stick', 'en-US', 60, [(10, 'cross')], 7),
    ('ftp-partial', '7:partial', 'en-US', 60, [(10, 'cross')], 7),
    ('ftp-found', '7', 'en-US', 60, [(10, 'cross')], 7),
    ('method-pick-ftp', '2', 'en-US', 60, [(10, 'right'), (18, 'cross')], 7),
    ('ftp-de-empty', '7:no-stick', 'de-DE', 60, [(10, 'cross')], 7),
    ('ftp-ru', '7', 'ru-RU', 60, [], 7),
    ('ftp-ja-partial', '7:partial', 'ja-JP', 60, [(10, 'cross')], 7),
    ('ftp-zh', '7', 'zh-CN', 60, [], 7),
    # Installing reached by FTP speaks of /data/ps5proton/install, not a drive: under way, and a damaged piece
    ('ftp-installing', '7', 'en-US', 60, [(10, 'cross'), (20, 'cross')], 4),
    ('ftp-installing-damaged', '7:damaged', 'en-US', 500, [(10, 'cross'), (20, 'cross')], 4),
    ('ftp-installing-fr', '7', 'fr-FR', 60, [(10, 'cross'), (20, 'cross')], 4),
    ('method-fr', '2', 'fr-FR', 60, [], 2),
    ('method-ru', '2', 'ru-RU', 60, [], 2),
    ('method-ja', '2', 'ja-JP', 60, [], 2),
    # The console gate's walk (tools/run-launcher.py --setup): Welcome to Preparing, in Japanese, through the fake install
    # (each frame is at most 0.05 s of the tour's time: 700 frames cover the 8.6 s install at 15 fps or more)
    # The first session's cover (src/pw_setup.h): Start's frame, kept by the launcher, in every language; the title's
    # marks are drawn over it by the same C code (test_pw_setup's picture mode) as setup-cover-<language>-<stage>.png.
    *[('cover-' + tag, '5:cover', tag, 40, [], 5) for tag in COVER_LANGUAGES],
    ('walk-ja', '0', 'en-US', 700, [(30, 'cross'), (50, 'down'), (56, 'right'), (62, 'right'), (70, 'cross'),
                                     (100, 'cross'), (130, 'cross'), (150, 'cross')], 5),
    # The Steam guide's launcher pages ("steam:" runs PROTON_STEAM_GUIDE_TEST; the number is the page the log names)
    *[(f"steam-{test.split('@')[0].split(':')[0].split('#')[0]}-{tag}", 'steam:' + test, tag, 40, [], page)
      for tag in COVER_LANGUAGES for test, page in STEAM_PAGES.items()],
    *[(f'steam-failed-{word}', f'steam:failed:{word}', 'en-US', 40, [], 2) for word in STEAM_FAILURES],
]


def main():
    from PIL import Image, ImageStat
    subprocess.run(['ninja', '-C', str(BUILD), 'ps5-samples-host'], check=True, stdout=subprocess.DEVNULL)
    marks = OUT / 'cover-marks'
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cc', '-std=gnu11', '-O2', str(ROOT / 'tests/test_pw_setup.c'), str(ROOT / 'src/pw_setup.c'), '-lm',
                    '-o', str(marks)], check=True)
    script = APP / 'test-run.txt'
    backup = script.read_bytes() if script.exists() else None
    OUT.mkdir(parents=True, exist_ok=True)
    try:
        for name, test, language, frames, presses, step in RENDERS:
            script.write_text(f'frames {frames}\nscreenshot\nsamples ps5proton\n' +
                              ''.join(f'press ps5proton {f} {a}\n' for f, a in presses))
            steam = test.startswith('steam:')
            env = dict(os.environ, PROTON_SETUP_LANGUAGE=language)
            env.pop('PROTON_SETUP_TEST' if steam else 'PROTON_STEAM_GUIDE_TEST', None)
            env['PROTON_STEAM_GUIDE_TEST' if steam else 'PROTON_SETUP_TEST'] = test[6:] if steam else test
            run = subprocess.run([str(BUILD / 'ps5-samples-host')], capture_output=True, text=True, timeout=900, env=env)
            log = run.stdout + run.stderr
            (OUT / f'setup-{name}.log').write_text(log)
            assert run.returncode == 0 and 'ends: status 0' in log, f'{name}: {log[-1500:]}'
            steps = re.findall(r'steam guide: page (\d+)' if steam else r'setup: (?:starts at )?step (\d+)', log)
            assert steps and int(steps[-1]) == step, f'{name}: reached step {steps[-1:]} not {step}'
            assert 'setup: the sky' not in log, f'{name}: the sky failed'
            image = Image.open(APP / 'screenshots/ps5proton.ppm').convert('RGB')
            assert max(ImageStat.Stat(image).stddev) > 8, f'{name}: blank picture'
            image.save(OUT / f'setup-{name}.png')
            if test == '5:cover':
                stages = range(len(COVER_STAGES)) if language == 'en-US' else (1,)
                for stage in stages:
                    marked = OUT / 'cover-marked.ppm'
                    subprocess.run([str(marks), str(ROOT), str(APP / 'screenshots/ps5proton.ppm'), str(marked), str(stage), '3'],
                                   check=True, stdout=subprocess.DEVNULL)
                    Image.open(marked).save(OUT / f'setup-{name}-{COVER_STAGES[stage]}.png')
                    marked.unlink()
            print(f'setup-{name}: step {step}, {frames} frames')
        if 'setup: language fr-FR' not in (OUT / 'setup-language-pick-french.log').read_text():
            raise AssertionError('choosing the third tile did not choose French')
        if 'setup: language pl-PL' not in (OUT / 'setup-language-pick-polish.log').read_text():
            raise AssertionError('choosing the last tile did not choose Polish')
        # The sessions' screens: the title's own drawing, from tests/test_pw_steam_screen.c's picture mode
        screens, sessions = OUT / 'steam-screens', OUT / 'steam-session'
        cuts = BUILD.parent / 'ps5/generated/setup/fonts'
        sessions.mkdir(exist_ok=True)
        subprocess.run(['cc', '-std=gnu11', '-O2', str(ROOT / 'tests/test_pw_steam_screen.c'), str(ROOT / 'src/pw_steam_screen.c'),
                        str(ROOT / 'src/pw_loading_art.c'), str(ROOT / 'src/pw_setup.c'), '-lm', '-o', str(screens)], check=True)
        subprocess.run([str(screens), str(ROOT / '.deps/hui/third_party/fonts'), str(cuts) if cuts.is_dir() else '-',
                        str(sessions)], check=True)
        for ppm in sorted(sessions.glob('*.ppm')):
            Image.open(ppm).save(OUT / ('setup-steam-session-' + ppm.stem[len('steam-'):] + '.png'))
            ppm.unlink()
        print(f'Steam guide session screens: {len(list(OUT.glob("setup-steam-session-*.png")))} pictures')
        if shutil.which('zbarimg'):
            for name in ('usb', 'method'):
                read = subprocess.run(['zbarimg', '-q', '--raw', str(OUT / f'setup-{name}.png')], capture_output=True, text=True)
                assert read.stdout.strip() == URL, f'setup-{name}: the QR code reads {read.stdout.strip()!r}'
            print(f'QR code: reads {URL}')
        else:
            print('QR code: not read back (no zbarimg)')
        print(f'Setup pictures: {OUT}')
    finally:
        if backup is None:
            script.unlink(missing_ok=True)
        else:
            script.write_bytes(backup)


if __name__ == '__main__':
    main()
