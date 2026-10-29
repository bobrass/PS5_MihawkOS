# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/start_stages.py lines up the title's spawn line, the helper's start stamps and Wine's module lines on one clock;
the fixture is two starts in the formats wine/ps5/pw_wine_process.c and native/proton_wine_child.c print."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import start_stages  # noqa: E402

OWNER = [
    '[MihawkOS] wine-ps5: child spawn service=0x1',
    '[MihawkOS] wine-ps5: spawn native=785 request_ms=1000 lock=0 read=4 answered=310 connected=420 attempts=2 held=425 '
    'sent=430 ready=600',
    '[MihawkOS] wine-ps5: spawn native=786 request_ms=5000 lock=50 read=54 answered=300 connected=330 attempts=1 held=335 '
    'sent=340 ready=500',
    '[MihawkOS] wine-ps5: spawn native=787 request_ms=9000 lock=0 read=-1 answered=-1 connected=-1 attempts=0 held=-1 '
    'sent=10 ready=200',
    '[MihawkOS] wine-ps5: spawn native=788 request_ms=20000 lock=5 read=-1 answered=-1 connected=-1 attempts=0 held=-1 '
    'sent=3 ready=150 warm=1',
]
CHILD_788 = """wine-child: start at=main ms=12000
wine-child: start at=control ms=12080
wine-child: start at=entry ms=12081
wine-child: start at=bootstrap ms=20004
wine-child: start at=ntdll-loaded ms=20060
"""
CHILD_785 = """wine-child: entry
wine-child: start at=main ms=1350
wine-child: start at=control ms=1424
wine-child: start at=entry ms=1426
wine-child: start at=bootstrap ms=1431
wine-child: start at=payload-entry ms=1500
wine-child: start at=ntdll-loading ms=1520
wine-child: start at=ntdll-loaded ms=1540
wine-child: start at=wine-main ms=1610
1.700:0020:0024:trace:loaddll:build_module Loaded L"C:\\\\windows\\\\system32\\\\iexplore.exe" at 0000000140000000: builtin
1.950:0020:0024:trace:loaddll:build_module Loaded L"C:\\\\windows\\\\system32\\\\user32.dll" at 000000DFFF650000: builtin
wine-child: life summary ms=2010 server_calls=900 server_ms=120 file_read_calls=40 file_read_ms=9 file_open_calls=30 file_open_ms=6 file_attributes_ms=1 file_information_ms=2 section_map_calls=12 section_map_ms=33 section_fault_calls=0 section_fault_ms=0
wine-child: payload ending monotonic_ms=2050
"""


class StartStages(unittest.TestCase):
    def test_points_steps_and_medians(self):
        record = start_stages.summary(OWNER, {785: CHILD_785, 786: '', 788: CHILD_788})
        self.assertEqual(record['starts'], 4)
        self.assertEqual(record['warm_starts'], 1)
        first = record['helpers']['785']
        self.assertEqual(first['attempts'], 2)
        # Every point after its request, title and helper and Wine on one clock.
        self.assertEqual(first['points']['answered'], 310)
        self.assertEqual(first['points']['main'], 350)
        self.assertEqual(first['points']['first-dll'], 700)
        self.assertEqual(first['points']['last-dll'], 950)
        self.assertEqual(first['points']['ending'], 1050)
        self.assertEqual(list(first['points']), sorted(first['points'], key=first['points'].get))
        # Steps are the gaps in time order, named by where they end: elfldr's answer, then the carrier's main.
        self.assertEqual(first['steps']['answered'], 306)
        self.assertEqual(first['steps']['main'], 40)
        self.assertEqual(first['steps']['first-dll'], 90)
        self.assertEqual(sum(first['steps'].values()), first['points']['ending'])
        self.assertEqual(first['life']['section_map_ms'], 33)
        self.assertNotIn('ms', first['life'])
        # A start without a helper log keeps the title's points; steps the title did not reach (-1) are absent.
        self.assertEqual(record['helpers']['786']['points']['ready'], 500)
        self.assertIsNone(record['helpers']['786']['life'])
        self.assertNotIn('read', record['helpers']['787']['points'])
        self.assertEqual(record['median']['points']['ready'], 500)
        self.assertEqual(record['median']['points']['main'], 350)
        self.assertEqual(record['median']['steps']['lock'], 0)
        # A warm start: its carrier was made before the request; those points are negative and not steps.
        warm = record['helpers']['788']
        self.assertTrue(warm['warm'] and not first['warm'])
        self.assertEqual(warm['points']['main'], -8000)
        self.assertNotIn('main', warm['steps'])
        self.assertEqual(warm['steps']['bootstrap'], 1)
        self.assertEqual(record['median_warm']['points']['ready'], 150)
        self.assertNotIn('main', record['median_warm']['steps'])

    def test_no_stamped_start(self):
        self.assertIsNone(start_stages.summary(['[MihawkOS] wine-ps5: child spawn service=0x1'], {}))

    def test_run_folder_and_compare(self):
        with tempfile.TemporaryDirectory() as folder:
            Path(folder, 'r.log').write_text('\n'.join(OWNER) + '\n')
            Path(folder, 'r-wine-child-785.log').write_text(CHILD_785)
            record = start_stages.from_run(folder, 'r')
            self.assertEqual(record['helpers']['785']['points']['wine-main'], 610)
            a, b = Path(folder, 'a.json'), Path(folder, 'b.json')
            a.write_text(json.dumps({'start_stages': record}))
            b.write_text(json.dumps({'start_stages': start_stages.summary(OWNER[:2], {})}))
            text = start_stages.compare(record, start_stages.summary(OWNER[:2], {}))
            self.assertIn('answered', text)
            self.assertEqual(start_stages.main(['x', 'compare', str(a), str(b)]), 0)
            self.assertIsNone(start_stages.summary(OWNER[:2], {})['median_warm'])


if __name__ == '__main__':
    unittest.main()
