#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/run-shared-charge.py on the PS5 Pro: what counts as the system saying it is short of memory, and that the ramp
still has its ceiling and its refusals. The runner's console work is run on the console; this checks what does not need one."""
import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
sys.path.insert(0, str(ROOT / 'tools'))
SOURCE = ROOT / 'tools' / 'run-shared-charge.py'


def runner():
    spec = importlib.util.spec_from_file_location('run_shared_charge', SOURCE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class LowMemory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.module = runner()
        except ModuleNotFoundError as error:   # the shared console tooling is not beside this tree
            raise unittest.SkipTest(str(error))

    def test_the_system_saying_it_is_short(self):
        for line in ('<118>[SceShellCore] Not enough memory to start the application',
                     '[KERNEL] WARN: low memory, reclaiming', 'process killed: out of memory', 'memory is full',
                     'flexible memory exceeded the limit'):
            self.assertTrue(self.module.LOWMEM.search(line), line)

    def test_ordinary_lines_are_not_it(self):
        for line in ('<118>[SceShellCore] VM Stats: RSS 142.6 1019.8, kernel 364.2 179.4, wire count 461.1 204.7',
                     '[KERNEL] INFO: FMEM size: 0x40000000', 'we allow memory mapping here', 'hw.physmem is not approved',
                     '[shared-charge] step=3 phase=held mappings=3 resident_kib=196608', 'flowing memoryless stream'):
            self.assertFalse(self.module.LOWMEM.search(line), line)


class Safety(unittest.TestCase):
    text = SOURCE.read_text()

    def test_the_pro_has_a_ceiling_and_a_refusal_each_way(self):
        for needle in ('ceiling_mib = base', "state['stop_pending'] = True", 'this console prints no RSS limit: not the Pro',
                       'not the standard PS5; not sent', "'title ended'", "args.console == 'pro'", "state.get('offset_mib', 0.0)",
                       "the probe's census of other processes is empty", 'ceiling_step\'] >= 2', "'[MihawkOS]' not in line"):
            self.assertIn(needle, self.text)

    def test_the_probe_is_still_refused_the_sdk_kernel_write(self):
        self.assertIn('the probe can reach the SDK kernel write or fork: not sent', self.text)


if __name__ == '__main__':
    unittest.main()
