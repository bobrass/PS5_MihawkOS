#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""No console: the installer slice reads each check once, from plain and JSON-escaped log lines alike."""
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('slice', Path(__file__).resolve().parents[1] / 'tools/run-installer-slice.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
checks, verdict = runner.parse('x "PS5_PROTON_INSTALLER_CHECK vcredist clean FAIL 5 ms: removed 24\\n"\n'
                               'PS5_PROTON_INSTALLER_CHECK vcredist clean FAIL 5 ms: removed 24\n'
                               'PS5_PROTON_INSTALLER FAIL: vcredist, 3 checks\\n"')
assert checks == [{'step': 'clean', 'verdict': 'FAIL', 'ms': 5, 'detail': 'removed 24'}], checks
assert verdict == {'passed': False, 'summary': 'vcredist, 3 checks'}, verdict
print('installer slice parse: PASS')
