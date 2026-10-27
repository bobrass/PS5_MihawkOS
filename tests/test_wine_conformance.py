#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""No console: check the differential verdict cannot turn missing evidence into a pass."""
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('wine_tests', Path(__file__).resolve().parents[1] / 'tools/run-wine-tests.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
context = {'arch': 'i386', 'test_sha256': 'one', 'wine_revision': 'two', 'batch_header_sha256': 'three'}

def entry(status='complete', failures=(), arch='i386'):
    return {'status': status, 'context': dict(context, arch=arch), 'failures': list(failures)}

failure = {'file': 'virtual.c', 'line': 20, 'text': 'wrong protection'}
assert runner.compare_records({'virtual': entry()}, {'virtual': entry()}, {})['passed']
assert not runner.compare_records({}, {}, {})['passed']
assert not runner.compare_records({}, {'virtual': entry()}, {})['passed']
assert not runner.compare_records({'virtual': entry()}, {}, {})['passed']
assert not runner.compare_records({'virtual': entry()}, {'virtual': entry(arch='x86_64')}, {})['passed']
assert not runner.compare_records({'virtual': entry()}, {'virtual': entry('timeout')}, {})['passed']
assert not runner.compare_records({'virtual': entry('timeout')}, {'virtual': entry('timeout')}, {})['passed']
assert runner.compare_records({'virtual': entry(failures=[failure])}, {'virtual': entry(failures=[failure])}, {})['passed']
assert not runner.compare_records({'virtual': entry(failures=[failure])}, {'virtual': entry(failures=[failure, failure])}, {})['passed']
for reason in ['OPEN: broken mapper', 'no child processes (patch 0550)', 'winedevice cannot start']:
    assert not runner.compare_records({'virtual': entry()}, {'virtual': entry(failures=[failure])},
                                     {'virtual': {'virtual.c:20': reason}})['passed']
assert runner.compare_records({'virtual': entry()}, {'virtual': entry(failures=[failure])},
                              {'virtual': {'virtual.c:20': 'qualified platform limitation'}})['passed']
# A file whose PC run cannot complete passes only complete on the console without a failure line.
assert runner.compare_records({'volume': entry('incomplete')}, {'volume': entry()}, {}, {'volume': 'oracle crash'})['passed']
assert not runner.compare_records({'volume': entry('incomplete')}, {'volume': entry(failures=[failure])}, {}, {'volume': 'x'})['passed']
assert not runner.compare_records({'volume': entry('incomplete')}, {'volume': entry('timeout')}, {}, {'volume': 'x'})['passed']
assert not runner.compare_records({'volume': entry('incomplete')}, {'volume': entry()}, {})['passed']
# Slice provenance is preserved only if every slice came from one test build.
slices = {'d3d11@0-1': entry(), 'd3d11@1-2': entry()}
assert runner.merge_slices(slices, 'd3d11')['d3d11']['context'] == context
slices['d3d11@1-2'] = entry(arch='x86_64')
assert not runner.merge_slices(slices, 'd3d11')['d3d11']['context']
text = ('virtual.c:20: Test failed: wrong protection\n'
        '0020:virtual: 5 tests executed (0 marked as todo, 0 as flaky, 1 failure), 0 skipped.\n')
parsed = runner.parse(text)
assert parsed['status'] == 'complete' and parsed['failure_count'] == 1
assert runner.parse('winetest-batch: begin virtual\n')['status'] == 'incomplete'
# A child that re-runs the whole batch (its argv[1] was the list) must not overwrite or pass the file.
segments, order, repeated = runner.split_batch('winetest-batch: begin info\nwinetest-batch: begin file\n'
                                               'winetest-batch: end file 0\nwinetest-batch: begin info\n')
assert order == ['info', 'file'] and repeated == {'info'}
runner.ARCH = 'i386'
assert runner.test_exe('ntdll').name == 'ntdll_test32.exe'
runner.ARCH = 'x86_64'
assert runner.test_exe('ntdll').name == 'ntdll_test.exe'
a, b = entry(), entry()
a['summary'] = {'skipped': 0}
b['summary'] = {'skipped': 2}
assert not runner.compare_records({'virtual': a}, {'virtual': b}, {})['passed']
a['summary']['skipped'] = 2
assert runner.compare_records({'virtual': a}, {'virtual': b}, {})['skipped_checks']
spec = importlib.util.spec_from_file_location('console_run', Path(__file__).resolve().parents[1] / 'tools/run-console.py')
console = importlib.util.module_from_spec(spec)
spec.loader.exec_module(console)
assert console.probe_completed('PASS', None)
assert not console.probe_completed('PASS', 'no return before the watchdog')
assert not console.probe_completed('FAIL', None)
assert not console.probe_completed(None, None)
assert console.native_close_failures('wine-ps5: child native=10 service=0x1 closed=0\n') == []
failed_close = 'wine-ps5: child native=8001 service=0x1 closed=0xffffffff\n'
assert console.native_close_failures(failed_close + failed_close + 'PS5_PROTON_OWNER_BOOTSTRAP PASS\n') == [
    {'pid': 8001, 'status': 4294967295}]
assert console.native_close_failures('wine-ps5: child native=11 service=0x1 closed=-1\n') == [
    {'pid': 11, 'status': -1}]
assert console.broker(None) is None
assert not console.broker('gpu-wine stopped=1 host_peak=1 local_peak=2')['wedged']
unsafe = console.broker('gpu-broker wedged=1 reason=wine_gpu_service_still_live\n'
                       'gpu-wine stopped=1 host_peak=1 local_peak=2')
assert unsafe['wedged'] and unsafe['failure_reasons'] == ['wine_gpu_service_still_live']
assert runner.merge_slices({'d3d11@0-1': entry('fault'), 'd3d11@1-2': entry()}, 'd3d11')['d3d11']['status'] == 'fault'

a, b = entry(), entry()
a['context']['graphics_sha256'] = 'one'
b['context']['graphics_sha256'] = 'two'
assert not runner.compare_records({'d3d11': a}, {'d3d11': b}, {})['passed']
# FILE@ARCH skips one architecture only (quartz's 32-bit helpers left stopped, 2026-10-25); FILE skips both
saved_skip = runner.load
runner.load = lambda path: {'q': {'a@i386': 'r', 'b': 's'}} if path == runner.SKIP else saved_skip(path)
assert runner.skips('q', 'i386') == {'a': 'r', 'b': 's'} and runner.skips('q', 'x86_64') == {'b': 's'}
runner.load = saved_skip

# a file accepted unfinished for a recorded cause is a known limitation, not unfinished (dinput hid: no plug-and-play)
v = runner.compare_records({'hid': entry()}, {'hid': entry('incomplete')}, {}, None, {'hid': 'no plug-and-play'})
assert v['passed'] and not v.get('unfinished'), v
print('wine conformance verdict: PASS (provenance, missing evidence, timeouts, repeated failures, skips, slices, watchdog rejection)')
