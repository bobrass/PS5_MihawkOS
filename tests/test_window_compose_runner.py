# SPDX-License-Identifier: GPL-3.0-or-later
"""A missing sample or capture cannot produce a displayed-window pass."""
import importlib.util
import tempfile
import unittest
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('runner', ROOT / 'tools/run-window-compose.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class WindowCaptureChecks(unittest.TestCase):
    def test_complete_pixels(self):
        with tempfile.TemporaryDirectory() as folder:
            old = runner.ROOT
            runner.ROOT = Path(folder)
            try:
                background = runner.ROOT / '.deps/staged/runtime/ge11-7/lib/wine/prefix/drive_c/windows/web/wallpaper/proton.bmp'
                background.parent.mkdir(parents=True)
                Image.new('RGB', (1920, 1080)).save(background)
                captures, lines = [], []
                for number, count in enumerate((4, 4, 3, 4, 3, 4, 3, 3, 3, 3, 3, 3, 3, 3), 1):
                    image = Image.new('RGB', (1920, 1080), (0, 0, 0))
                    path = f'{number}.png'
                    captures.append(path)
                    for x in range(count):
                        image.putpixel((x + 2, 3), (30, 110, 210))
                        lines.append(f'window-compose capture={number} phase=test sample={x+2},3 '
                                     'expected=30,110,210 result=PASS')
                    image.save(runner.ROOT / path)
                log = '\n'.join(lines)
                background.unlink()
                self.assertFalse(runner.display_verdict(captures, log)[1])
                self.assertTrue(runner.display_verdict(captures, log+'\nwindow-compose desktop_colour=0,0,0')[1])
                Image.new('RGB', (1920, 1080)).save(background)
                self.assertTrue(runner.display_verdict(captures, log)[1])
                self.assertFalse(runner.display_verdict(captures[:5], log)[1])
                # The GPU probe's seven captures, the sixth another process's image in the owner's window, the seventh a
                # window larger than the screen: it covers the desktop, whose colour is not checked there.
                graphics, glines = [], []
                for number, count in enumerate((4, 4, 3, 4, 3, 3, 4), 1):
                    image = Image.new('RGB', (1920, 1080), (30, 180, 70) if number == 7 else (0, 0, 0))
                    path = f'g{number}.png'
                    graphics.append(path)
                    for x in range(count):
                        image.putpixel((x + 2, 3), (30, 110, 210))
                        glines.append(f'window-compose capture={number} phase=test sample={x+2},3 '
                                      'expected=30,110,210 result=PASS')
                    image.save(runner.ROOT / path)
                self.assertTrue(runner.display_verdict(graphics, '\n'.join(glines), graphics=True)[1])
                self.assertFalse(runner.display_verdict(graphics[:6], '\n'.join(glines), graphics=True)[1])
                self.assertFalse(runner.display_verdict(graphics, '\n'.join(glines)[:-1], graphics=True)[1])
                self.assertFalse(runner.display_verdict(graphics[6:] + graphics[1:], '\n'.join(glines), graphics=True)[1])
                self.assertTrue(runner.display_verdict(captures, log + '\n' + log)[1])
                self.assertFalse(runner.display_verdict(captures[:-1], log)[1])
                self.assertFalse(runner.display_verdict(captures, '\n'.join(lines[1:]))[1])
                self.assertFalse(runner.display_verdict(captures, log.replace('result=PASS', 'result=FAIL', 1))[1])
                Image.new('RGB', (1920, 1080)).save(runner.ROOT / captures[0])
                self.assertFalse(runner.display_verdict(captures, log)[1])
            finally:
                runner.ROOT = old


if __name__ == '__main__':
    unittest.main()
