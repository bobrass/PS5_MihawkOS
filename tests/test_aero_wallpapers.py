#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/make-aero-wallpapers.py, Bubble Earth (the desktop's wallpaper since 2026-10-25), Glass City and Aqua World: the
same pixels on every run (they are seeded, and the build redraws Bubble Earth only when a generator changes), the size
asked for, sky above and grass or water below."""
import importlib.util
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('aero_wallpapers', ROOT / 'tools/make-aero-wallpapers.py')
aero = importlib.util.module_from_spec(spec)
spec.loader.exec_module(aero)


class AeroWallpapers(unittest.TestCase):
    def test_scenes(self):
        self.assertEqual(set(aero.SCENES), {'bubble-earth', 'glass-city', 'aqua-world'})

    def test_deterministic_sized_sky_over_ground(self):
        for name in aero.SCENES:
            first, second = aero.render(name, 320, 180), aero.render(name, 320, 180)
            self.assertEqual(first.size, (320, 180), name)
            self.assertEqual(first.mode, 'RGB', name)
            a = np.asarray(first).astype(int)
            self.assertTrue((a == np.asarray(second)).all(), name)
            top = a[2:20, 100:220].reshape(-1, 3).mean(0)
            self.assertGreater(top[2], top[0] + 60, name)   # a blue sky at the top

    def test_bubble_earth_stands_on_grass(self):
        a = np.asarray(aero.render('bubble-earth', 320, 180)).astype(int)
        bottom = a[-8:-2, 160:300].reshape(-1, 3).mean(0)   # the meadow, right of the Earth
        self.assertGreater(bottom[1], bottom[2] + 40)


if __name__ == '__main__':
    unittest.main()
