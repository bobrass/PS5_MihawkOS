#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/memory_breakdown.py: a run's counted memory split into code made, code taken and the rest, from the lines the
console and the helpers print. The fixture lines are the formats the sources print (src/pw_vm_census.c,
native/proton_wine_child.c, native/proton_wine.cpp)."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import memory_breakdown as mb  # noqa: E402

RUN = '\n'.join([
    'Wine: firmware 10.01 installed (the system reports 12.09); the memory guard\'s limit is 870 MiB',
    '<118>[SceShellCore] VM Stats: RSS 150.0 1019.8, kernel 358.8 169.3, wire count 478.5 194.9, swap out 62.9, page table CPU 5209/9216 GPU 167/9216',
    '<118>[SceShellCore] VM Stats: RSS 1000.0 1019.8, kernel 368.1 181.1, wire count 568.2 212.9, swap out 70.5, page table CPU 8708/9216 GPU 183/9216',
    'Wine: system memory 900 MiB (helpers 700 MiB in 12, the largest 100 MiB in process 5), peak 900 MiB; 0 s of the last 30 at the 870 MiB limit; shared code copies beyond the first 12 MiB (what counting them once would take off)',
    'Wine: system memory 950 MiB (helpers 750 MiB in 12, the largest 100 MiB in process 5), peak 950 MiB; 0 s of the last 30 at the 870 MiB limit; shared code copies beyond the first 20 MiB (what counting them once would take off)',
    'Wine: system memory 940 MiB (helpers 740 MiB in 12, the largest 100 MiB in process 5), peak 950 MiB; 0 s of the last 30 at the 870 MiB limit; shared code copies beyond the first 18 MiB (what counting them once would take off)',
])
BIG = '\n'.join([
    '12018.617:0110:0114:trace:loaddll:build_module Loaded L"C:\\\\Program Files\\\\X\\\\app.exe" at 0000000140000000: native',
    'wine-child: memory resident=100 MiB mapped=1000 MiB (status 0) images=1 holding 2 MiB',
    'wine-child: executable protection refused=0 conversions=10 converted_kib=20480 convert_failed=0 exec_refused=0',
    'wine-child: shared code copies shared=100 shared_kib=10240 offered=9 mismatched=0 failed=0 private_writes=0 private_kib=0 mapped_kib=14336',
    'wine-child: vm resident=1 MiB direct_resident=2 reserved_resident=0 flexible=90 direct=40 other=5 MiB mappings=3 groups=2 top x',
    'wine-child: vm flexible-kinds r.s=4/0.1/0.0 rw.x=2/50.0/30.5',
    'wine-child: vm flexible-big 1000+40MiB:01/3:anon 2000+10MiB:01/3:anon',
])
SMALL = '\n'.join([
    'wine-child: memory resident=14 MiB mapped=50 MiB (status 0)',
    'wine-child: vm flexible-kinds r.s=4/0.1/0.0',
])
NOTHING = 'wine-child: entry\n'


class Breakdown(unittest.TestCase):
    def test_helper_row_splits_the_counted_figure(self):
        row = mb.helper_row(BIG)
        self.assertEqual((row['program'], row['counted']), ('app.exe', 100))
        self.assertAlmostEqual(row['made'], 20.0)
        self.assertAlmostEqual(row['taken'], 10.0)
        self.assertAlmostEqual(row['rest'], 70.0)
        self.assertEqual(row['kinds'][('rw', 'x')], (2, 50.0, 30.5))
        self.assertEqual(row['flexbig'].split()[0], '1000+40MiB:01/3:anon')

    def test_a_helper_without_a_counted_figure_is_left_out(self):
        self.assertIsNone(mb.helper_row(NOTHING))

    def test_rest_never_goes_negative(self):
        row = mb.helper_row(BIG.replace('resident=100 MiB', 'resident=20 MiB'))
        self.assertEqual(row['rest'], 0.0)

    def test_run_totals_and_the_census_with_copies_counted_once(self):
        s = mb.summarize(RUN, [BIG, SMALL, NOTHING])
        self.assertEqual((s['firmware'], len(s['helpers']), s['counted']), ('10.01', 2, 114))
        self.assertAlmostEqual(s['made'] + s['taken'] + s['rest'], 114.0)
        self.assertEqual((s['rss_peak'], s['limit'], s['swap_max'], s['pt_peak'], s['pt_pool']), (1000.0, 1019.8, 70.5, 8708, 9216))
        self.assertEqual((s['beyond_first_last'], s['beyond_first_max']), (18, 20))
        self.assertAlmostEqual(s['mapped'], 14.0)
        self.assertAlmostEqual(s['rss_once'], 980.0)   # the title's figure, 20 MiB at most, not the running totals (10)
        self.assertAlmostEqual(s['headroom_once'], 39.8)
        self.assertEqual(s['kinds'][('r', 's')][0], 8)
        text = '\n'.join(mb.render('fixture', s))
        self.assertIn('copies counted once, at most: RSS peak 1000 -> 980 MiB, +40 MiB against the limit', text)
        self.assertIn('registry objects the helpers map now: 14 MiB', text)
        self.assertIn('rw >=8M', text)

    def test_a_run_without_console_lines_still_summarizes(self):
        s = mb.summarize('Wine: firmware 10.01 installed', [SMALL])
        self.assertNotIn('rss_peak', s)
        self.assertTrue(mb.render('fixture', s))

    def test_the_sources_still_print_the_lines(self):
        census = (ROOT / 'src' / 'pw_vm_census.c').read_text()
        child = (ROOT / 'native' / 'proton_wine_child.c').read_text()
        self.assertIn('"%s%s.%c=%u/%llu.%llu/%llu.%llu"', census)
        for text in ('wine-child: vm flexible-kinds %s', 'wine-child: vm flexible-big %s', 'wine-child: memory resident=%llu MiB',
                     'shared code copies shared=%llu shared_kib=%llu offered=%llu', 'private_kib=%llu mapped_kib=%llu'):
            self.assertIn(text, child)


if __name__ == '__main__':
    unittest.main()
