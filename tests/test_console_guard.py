#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The console guard against a fake clock and fake services; never touches the console.

The storage fixtures are verbatim klog lines (klog/ itself is never committed), each with the file and line it came
from. PROTON_KLOG_REPLAY=1 also scans every top-level klog/*.log (KlogReplay, several GiB).
"""
import ftplib
import importlib.util
import io
import json
import os
import re
import socket
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import console_guard as guard  # noqa: E402


def load_tool(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


run_console = load_tool('run_console', 'run-console.py')
verify_title = load_tool('verify_title', 'verify-title.py')
battery = load_tool('battery', 'run-battery.py')

IDLE = 'ok procs count=0 pids='
CENSUS = 'pid=95 ppid=1 name=websrv.elf\npid=77 ppid=1 name=SceSpZeroConf\n'
PING_42 = 'ok ps5vkctl 1 pid=42'
EXEC_AT = '<5582> EXEC /system/vsh/app/NPXS40112/eboot.bin [unspecified], vm#0, dmem#0 abi=native category=unknown <%d msec>'

# A klog of 2026-10-13, six lines (the first boot after the 10-13 shutdowns, before format #1).
PROF_08 = [
    '[IOD] process_completion: error opc: 0x12, ec: 0x71, ering: 0x3, loc: 0xd5bc, eaddr: 0x87800041b617, rid: 0x3d5bc',
    '[IOD] process_completion: conv error: 0x71 -> 0x5',
    'bfs_device_nsid1_read_ [sid=0x5460007b] -> errno=5, a53error=0x5, val=0x0000',
    '<118>[PS][fserr] 0x80020005,0x00000001,ssd0.user,/user,SceShellCore,sceBgDailyChckerJobQueue',
    '<118>[PlayGoCore][ReportCorruptPackageError] begin (PPSA99170)',
    '[KERNEL] WARNING: Decrypt error in SELF block.  Retrying...(1)',
]
# A klog of 2026-10-14, four lines (2 h before reinstall #2).
NOAMD_01 = [
    '[IOD] process_completion: error opc: 0x12, ec: 0x71, ering: 0x3, loc: 0x76f0, eaddr: 0x878000107ed4, rid: 0x376f0',
    'bfs_device_nsid1_read_ [sid=0x46f00197] -> errno=5, a53error=0x5, val=0x0000',
    '<118>[PS][fserr] 0x80020005,0x00000001,ssd0.user,/user,SceShellCore,SceLncTerminateMountRootThread',
    '<118>[PlayGoCore][ReportCorruptPackageError] begin ((null))',
]
# Routine lines that must not block, from klogs of 2026-10-13 and 2026-10-14: a repaired fsck, a conversion line that
# always follows an error line, and a boot log's memory line.
BENIGN = [
    '<118>[PS][fserr] ClearSlotInfoCache() [PPSA99170] is not registered',
    '<118>need fsck [**************/PPSA99170/download0.dat]',
    '<118>sceFsUfsFsck_ckfini: ***** FILE SYSTEM MARKED CLEAN *****',
    '[IOD] process_completion: conv error: 0x71 -> 0x5',
    '<118>[SceShellCore] FMEM  26.5/ 144.9 582.9/3525.6 NPXS40087 SceShellUI',
]

class GuardCase(unittest.TestCase):
    """A temporary guard state, a fake clock and a fake control payload."""
    fake_clock = True

    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.dir = Path(self.scratch.name)
        self.now = 2000000000.0
        self.sleeps = []
        self.ping = Mock(return_value=PING_42)
        self.procs = Mock(return_value=IDLE)
        self.sync = Mock(return_value='ok sync ms=12')
        self.census = Mock(return_value=SimpleNamespace(returncode=0, stdout=CENSUS))
        self.console = SimpleNamespace(ps5vkctl_command=self.command,
                                       load_settings=lambda: {'host': '127.0.0.1', 'klog_port': 1, 'ctl_port': 2})
        contexts = [
            patch.object(guard, 'STATE', self.dir / 'state.json'),
            patch.object(guard, 'LAST_RUN', self.dir / 'last-run'),
            patch.object(guard, 'KLOG', self.dir / 'klog'),
            patch.object(guard.subprocess, 'run', self.census),
            patch.dict(sys.modules, {'ps5_console': self.console}),
        ]
        if self.fake_clock:
            contexts += [patch.object(guard.time, 'time', lambda: self.now),
                         patch.object(guard.time, 'monotonic', lambda: self.now),
                         patch.object(guard.time, 'sleep', self.sleep)]
        for context in contexts:
            context.start()
            self.addCleanup(context.stop)
        guard.new_boot()
        guard.after_launch()

    def command(self, settings, command, timeout=60.0):
        return {'ping': self.ping, 'procs': self.procs, 'sync': self.sync}[command.split()[0]]()

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds

    def finish(self, clean):
        guard.finished(clean)
        os.utime(guard.LAST_RUN, (self.now, self.now))

    def ready(self):
        return guard.before_launch(readiness=lambda: guard.wait_ready({}))


class ReadinessTests(GuardCase):
    def test_clean_run_has_no_fixed_delay_and_still_checks_processes(self):
        self.finish(True)
        result = self.ready()
        self.assertTrue(result['ready'])
        self.assertEqual(self.sleeps, [])
        self.assertEqual(self.procs.call_count, 2)
        self.census.assert_called_once()
        self.assertEqual(guard.load()['launches'], 1)

    def test_unverified_cleanup_keeps_display_settle(self):
        self.finish(False)
        result = self.ready()
        self.assertEqual(self.sleeps, [8.0])
        self.assertEqual(result['settle_seconds'], 8.0)
        self.census.assert_called_once()

    def test_new_launch_invalidates_previous_cleanup(self):
        self.finish(True)
        guard.after_launch()
        self.assertFalse(guard.load()['cleanup']['clean'])
        self.assertEqual(guard.load()['launches'], 2)

    def test_drop_refuses_before_any_readiness_or_network(self):
        self.finish(True)
        guard.record_drop('test outage')
        with self.assertRaises(SystemExit): self.ready()
        self.ping.assert_not_called()
        self.procs.assert_not_called()
        self.census.assert_not_called()
        self.assertIsNotNone(guard.load()['dropped'])

    def test_drop_during_readiness_is_not_ignored(self):
        def ready():
            guard.record_drop('readiness outage')
            return {'ready': True}
        with self.assertRaises(SystemExit): guard.before_launch(readiness=ready)

    def test_leftover_helper_refuses_despite_clean_marker(self):
        self.finish(True)
        self.census.return_value.stdout += 'pid=90 ppid=95 name=SceSpZeroConf\n'
        with self.assertRaisesRegex(RuntimeError, 'outlived'): self.ready()
        self.assertEqual(self.procs.call_count, 1)

    def test_missing_or_failed_census_is_not_empty(self):
        self.finish(True)
        for result in (SimpleNamespace(returncode=1, stdout=CENSUS),
                       SimpleNamespace(returncode=0, stdout=''),
                       SimpleNamespace(returncode=0, stdout=CENSUS + 'pid=96 name=websrv.elf\n')):
            self.census.return_value = result
            with self.assertRaisesRegex(RuntimeError, 'Cannot list'): self.ready()

    def test_console_becoming_occupied_after_census_refuses(self):
        self.finish(True)
        self.procs.side_effect = [IDLE, 'ok procs count=1 pids=123']
        with self.assertRaisesRegex(RuntimeError, 'became occupied'): self.ready()

    def test_restart_waits_for_real_idle_instead_of_full_minute(self):
        self.finish(True)
        self.procs.side_effect = ['ok procs count=1 pids=123', IDLE, IDLE]
        self.ready()
        self.assertEqual(self.sleeps, [0.25])
        self.census.assert_called_once()

    def test_occupied_console_wait_is_bounded(self):
        self.finish(True)
        self.procs.return_value = 'ok procs count=1 pids=123'
        with self.assertRaisesRegex(RuntimeError, 'not idle'): self.ready()
        self.assertEqual(sum(self.sleeps), 60.0)
        self.census.assert_not_called()

    def test_unknown_readiness_cannot_enable_fast_launch(self):
        for result in (None, {}, {'ready': False}):
            with self.assertRaisesRegex(RuntimeError, 'not verified'):
                guard.before_launch(readiness=lambda: result)

    def test_legacy_probe_keeps_conservative_pacing(self):
        self.finish(True)
        guard.before_launch()
        self.assertEqual(self.sleeps, [60.0])
        self.procs.assert_not_called()


class ControlPayloadTests(GuardCase):
    """ps5vkctl answered this boot and is gone (rest mode, a manual run of 2026-10-14) or has another pid: a drop."""

    def setUp(self):
        super().setUp()
        self.finish(True)

    def test_first_answer_records_the_pid(self):
        self.ready()
        self.assertEqual(guard.load()['ps5vkctl_pid'], 42)

    def test_gone_after_answering_is_a_drop(self):
        self.ready()
        self.ping.side_effect = SystemExit('ps5vkctl: <address>:9111 did not answer')
        with self.assertRaises(SystemExit): self.ready()
        self.assertIn('pid 42', guard.load()['dropped']['why'])
        self.assertEqual(self.ping.call_count, 1 + 1 + guard.DROP_RETRIES)

    def test_answering_again_is_not_a_drop(self):
        self.ready()
        self.ping.side_effect = [SystemExit('busy'), PING_42, PING_42]
        self.ready()
        self.assertIsNone(guard.load()['dropped'])

    def test_a_new_pid_is_a_drop(self):
        self.ready()
        accepted = self.procs.call_count
        self.ping.return_value = 'ok ps5vkctl 1 pid=43'
        with self.assertRaises(SystemExit): self.ready()
        self.assertIn('pid 42 this boot and is now pid 43', guard.load()['dropped']['why'])
        self.assertEqual(self.procs.call_count, accepted)

    def test_no_answer_before_any_pid_only_needs_loading(self):
        self.ping.side_effect = SystemExit('ps5vkctl: 10.1.2.3:9111 did not answer')
        with self.assertRaisesRegex(RuntimeError, 'does not answer') as caught: self.ready()
        self.assertNotIn('10.1.2.3', str(caught.exception))
        self.assertIsNone(guard.load()['dropped'])
        self.procs.assert_not_called()

    def test_new_boot_forgets_the_pid(self):
        self.ready()
        guard.new_boot()
        self.assertNotIn('ps5vkctl_pid', guard.load())


class StorageTests(GuardCase):
    def test_fixture_lines(self):
        self.assertEqual(guard.storage_damage(PROF_08), PROF_08[:1] + PROF_08[2:])
        self.assertEqual(guard.storage_damage(NOAMD_01), NOAMD_01)
        self.assertEqual(guard.storage_damage(BENIGN), [])

    def test_damage_blocks_launches_and_writes(self):
        guard.record_storage('storage-run-01', guard.storage_damage(PROF_08))
        with self.assertRaisesRegex(SystemExit, 'storage damage'): guard.check_launch_allowed()
        with self.assertRaisesRegex(SystemExit, 'storage damage'): guard.begin_write('title')
        self.procs.assert_not_called()
        self.assertNotIn('deploys', guard.load())

    def test_block_survives_new_boot_and_keeps_the_first_evidence(self):
        guard.record_storage('storage-run-01', PROF_08[:1])
        guard.record_storage('storage-run-02', NOAMD_01[:1])
        guard.new_boot()
        self.assertEqual(guard.load()['storage']['run'], 'storage-run-01')
        with self.assertRaises(SystemExit): guard.check_launch_allowed()

    def test_a_person_clears_it(self):
        guard.record_storage('boot-x', NOAMD_01)
        record = guard.load()['storage']
        guard.clear_storage('Safe Mode screen read, title read back by hash')
        guard.check_launch_allowed()
        self.assertEqual(guard.load()['storage_cleared']['was'], record)


    def test_the_sibling_guards_new_boot_cannot_erase_the_blocks(self):
        guard.begin_write('runtime')
        guard.record_storage('storage-run-01', PROF_08[:1])
        shared = json.loads(guard.STATE.read_text())
        self.assertTrue(shared['dropped']['storage'])            # the one block the older guard honours
        self.assertFalse(set(guard.STICKY_KEYS) & set(shared))
        # ../PS5_Proton's older new_boot: its own keys only, written over the shared file.
        guard.STATE.write_text(json.dumps({'launches': 0, 'dropped': None, 'last_launch': 0.0,
                                           'boot_started': self.now, 'mode': 'legacy'}))
        state = guard.load()
        self.assertEqual((state['storage']['run'], state['deploys']['runtime']['verified']), ('storage-run-01', False))
        with self.assertRaisesRegex(SystemExit, 'storage damage'): guard.check_launch_allowed()

    def test_a_state_from_before_the_split_keeps_its_blocks(self):
        guard.STATE.write_text(json.dumps({'launches': 0, 'dropped': None, 'storage': {'run': 'old', 'lines': ['x']}}))
        guard.sticky_path().unlink(missing_ok=True)
        guard.after_launch()
        self.assertEqual(guard.load()['storage']['run'], 'old')
        self.assertNotIn('storage', json.loads(guard.STATE.read_text()))


class StateTests(GuardCase):
    def test_unreadable_state_refuses_everything(self):
        guard.STATE.write_text('{"dropped": nul')
        with self.assertRaisesRegex(SystemExit, 'unreadable'): guard.check_launch_allowed()
        with self.assertRaisesRegex(SystemExit, 'unreadable'): guard.begin_write('title')
        with self.assertRaisesRegex(SystemExit, 'unreadable'): guard.new_boot()

    def test_a_missing_state_is_a_fresh_boot(self):
        guard.STATE.unlink()
        self.assertIsNone(guard.load()['dropped'])

    def test_save_replaces_the_shared_file_and_keeps_the_link(self):
        shared = self.dir / 'other-checkout' / 'console-guard.json'
        shared.parent.mkdir()
        shared.write_text('{}')
        link = self.dir / 'link.json'
        link.symlink_to(shared)
        with patch.object(guard, 'STATE', link):
            guard.save({'launches': 3})
            self.assertTrue(link.is_symlink())
            self.assertEqual(json.loads(shared.read_text()), {'launches': 3})
        self.assertEqual(sorted(p.name for p in shared.parent.iterdir()), ['console-guard.json'])


class PerConsoleTests(GuardCase):
    def test_each_console_has_its_own_state_named_without_its_address(self):
        base, pro = guard.console_key({'host': '192.168.0.10'}), guard.console_key({'host': '192.168.0.11'})
        self.assertRegex(base, r'^[0-9a-f]{12}$')
        self.assertNotEqual(base, pro)
        with patch.object(guard, 'KEY', base):
            self.assertEqual(guard.state_path(self.dir).name, f'console-guard-{base}.json')
        with patch.object(guard, 'KEY', None):
            self.assertEqual(guard.state_path(self.dir).name, 'console-guard.json')
        self.console.load_settings = Mock(side_effect=SystemExit('PS5_HOST is not set'))
        self.assertIsNone(guard.console_key())

    def test_a_consoles_first_state_keeps_the_single_states_blocks_but_not_its_pid(self):
        legacy = self.dir / 'console-guard.json'
        legacy.write_text(json.dumps({'launches': 9, 'ps5vkctl_pid': 97, 'dropped': None, 'mode': 'lapy'}))
        (self.dir / 'console-guard-sticky.json').write_text(json.dumps({'storage': {'run': 'old', 'lines': ['x']}}))
        state = guard.load(self.dir / 'console-guard-aaaaaaaaaaaa.json')
        self.assertEqual(state['storage']['run'], 'old')
        self.assertNotIn('ps5vkctl_pid', state)
        self.assertEqual(state['launches'], 0)
        with patch.object(guard, 'STATE', self.dir / 'console-guard-aaaaaaaaaaaa.json'):
            with self.assertRaisesRegex(SystemExit, 'storage damage'): guard.check_launch_allowed()
            guard.clear_storage('checked')
            self.assertIsNone(guard.load()['storage'])
        self.assertEqual(json.loads((self.dir / 'console-guard-sticky.json').read_text())['storage']['run'], 'old')
        self.assertTrue((self.dir / 'console-guard-aaaaaaaaaaaa-sticky.json').exists())


class DeployTests(GuardCase):
    def test_a_deploy_is_unverified_until_read_back(self):
        guard.begin_write('title')
        with self.assertRaisesRegex(SystemExit, 'title on the console is not verified'): guard.check_launch_allowed()
        guard.begin_write('title')   # a second deploy is still allowed
        guard.record_verified('title', 'digest', 'read back')
        guard.check_launch_allowed()

    def test_a_probe_tools_writes_are_bracketed_however_they_end(self):
        with self.assertRaises(ValueError), guard.writing('probe'):
            raise ValueError('an upload failed')
        self.sync.assert_called_once()
        self.assertEqual(guard.load()['sync'], 'ok sync ms=12')
        guard.check_launch_allowed()                        # 'probe' only stamps the write
        guard.record_storage('storage-run-01', PROF_08[:1])
        with self.assertRaisesRegex(SystemExit, 'storage damage'), guard.writing('probe'):
            self.fail('a write ran on a storage block')

    def test_a_restore_that_did_not_finish_unverifies(self):
        guard.record_verified('title', 'digest', 'read back')
        guard.record_unverified('title', 'probe-01: the probe did not put the production title back')
        with self.assertRaisesRegex(SystemExit, 'probe-01'): guard.check_launch_allowed()

    def test_a_running_title_refuses_before_anything_is_marked(self):
        self.procs.return_value = 'ok procs app=5 title=PPSA99170 count=1 pids=5'
        with self.assertRaisesRegex(SystemExit, 'running an application'): guard.begin_write('runtime')
        self.assertNotIn('deploys', guard.load())
        self.assertNotIn('last_write', guard.load())

    def test_launches_settle_after_the_last_write(self):
        guard.begin_write('deploy-folder')
        self.now += 300                     # a long write: the settle counts from its end
        self.assertEqual(guard.end_write(), 'ok sync ms=12')
        self.now += 15
        guard.before_launch(readiness=lambda: {'ready': True})
        self.assertEqual(self.sleeps, [45.0])
        self.now += 60
        guard.before_launch(readiness=lambda: {'ready': True})
        self.assertEqual(self.sleeps, [45.0])

    def test_an_old_ps5vkctl_without_sync_still_settles(self):
        self.sync.side_effect = SystemExit('ps5vkctl: 10.1.2.3:9111 did not answer')
        reply = guard.end_write()
        self.assertTrue(reply.startswith('err '))
        self.assertNotIn('10.1.2.3', reply)
        self.assertEqual(guard.load()['last_write'], self.now)
        guard.before_launch(readiness=lambda: {'ready': True})
        self.assertEqual(self.sleeps, [60.0])

    def test_a_ps5vkctl_that_reads_sync_as_a_title_names_the_fix(self):
        self.sync.return_value = 'err usage: sync needs a nine-character title id'
        self.assertIn('tools/console-setup.py', guard.end_write())
        self.assertEqual(guard.load()['last_write'], self.now)

    def test_a_restart_needs_the_title_read_back_again(self):
        guard.record_verified('title', 'title-digest', 'read back')
        guard.record_verified('runtime', 'runtime-digest', 'deploy-wine')
        guard.new_boot()
        deploys = guard.load()['deploys']
        self.assertEqual((deploys['title']['verified'], deploys['title']['digest']), (False, 'title-digest'))
        self.assertTrue(deploys['runtime']['verified'])
        with self.assertRaisesRegex(SystemExit, 'verify-title'): guard.check_launch_allowed()
        self.assertIsNone(guard.refusal(guard.load(), deploys=False))


class KlogServer:
    """A klogsrv stand-in: sends its replay in pieces split mid-line, then stays silent."""

    def __init__(self, data):
        self.server = socket.create_server(('127.0.0.1', 0))
        self.port = self.server.getsockname()[1]
        self.done = threading.Event()
        threading.Thread(target=self.serve, args=(data,), daemon=True).start()

    def serve(self, data):
        connection, _ = self.server.accept()
        with connection:
            for start in range(0, len(data), 37):
                connection.sendall(data[start:start + 37])
            self.done.wait(10)

    def close(self):
        self.done.set()
        self.server.close()


class BootKlogTests(GuardCase):
    fake_clock = False

    def setUp(self):
        super().setUp()
        quiet = patch.object(guard, 'BOOT_QUIET', 0.2)
        quiet.start()
        self.addCleanup(quiet.stop)
        self.ping.side_effect = SystemExit('ps5vkctl: did not answer')

    def serve(self, lines):
        server = KlogServer(('\n'.join(lines) + '\n10.1.2.3 joined').encode())
        self.addCleanup(server.close)
        self.console.load_settings = lambda: {'host': '127.0.0.1', 'klog_port': server.port, 'ctl_port': 2}

    def new_boot(self):
        with patch.object(sys, 'argv', ['console_guard.py', 'new-boot']):
            return guard.main()

    def test_capture_returns_whole_lines_and_saves_them_redacted(self):
        self.serve(BENIGN + PROF_08)
        path = self.dir / 'boot.log'
        lines = guard.capture_boot_klog(self.console.load_settings(), path)
        self.assertEqual(lines, BENIGN + PROF_08 + ['<address> joined'])
        self.assertNotIn('10.1.2.3', path.read_text())

    def test_new_boot_saves_and_scans_the_replay(self):
        self.serve(BENIGN + NOAMD_01)
        self.assertEqual(self.new_boot(), 1)
        storage = guard.load()['storage']
        self.assertTrue(storage['run'].startswith('boot-'))
        self.assertEqual(storage['lines'], NOAMD_01)
        self.assertTrue((guard.KLOG / (storage['run'] + '.log')).is_file())
        self.assertEqual(guard.load()['launches'], 0)

    def test_a_clean_replay_records_the_boot(self):
        guard.record_drop('test outage')
        self.serve(BENIGN)
        self.assertEqual(self.new_boot(), 0)
        self.assertIsNone(guard.load()['dropped'])

    def reused_pid(self, launched_after_boot_s):
        state = guard.load()
        state.update(ps5vkctl_pid=42, launches=5, boot_started=self.time() - 8 * 3600,
                     last_launch=self.time() - 8 * 3600 + launched_after_boot_s)
        guard.save(state)
        guard.record_drop('rest mode')
        self.ping.side_effect = None

    @staticmethod
    def time():
        return guard.time.time()

    def test_refused_while_ps5vkctl_keeps_this_boots_pid(self):
        # Rest mode: the last launch's processes started 7 h into this boot, and the replay has them.
        self.reused_pid(7 * 3600)
        self.serve(BENIGN + [EXEC_AT % 25205000])
        self.assertEqual(self.new_boot(), 1)
        self.assertIsNotNone(guard.load()['dropped'])
        self.assertEqual(list(guard.KLOG.glob('boot-*.log')), [])

    def test_a_reused_pid_with_a_newer_boot_in_the_replay_is_a_restart(self):
        # 2026-10-17: restarted by hand, ps5vkctl got pid 97 again; the replay's processes started 106-112 s in.
        self.reused_pid(7 * 3600)
        self.serve(BENIGN + [EXEC_AT % 106032, EXEC_AT % 112427])
        self.assertEqual(self.new_boot(), 0)
        self.assertIsNone(guard.load()['dropped'])
        self.assertEqual(guard.load()['launches'], 0)

    def test_a_reused_pid_without_uptime_stamps_is_refused(self):
        self.reused_pid(7 * 3600)
        self.serve(BENIGN)
        self.assertEqual(self.new_boot(), 1)

    def test_a_reused_pid_with_no_launch_since_the_boot_is_refused(self):
        self.reused_pid(7 * 3600)
        state = guard.load()
        state['launches'] = 0
        guard.save(state)
        self.serve(BENIGN + [EXEC_AT % 106032])
        self.assertEqual(self.new_boot(), 1)

    def test_no_klog_leaves_the_state_as_it_was(self):
        guard.record_drop('test outage')
        closed = socket.create_server(('127.0.0.1', 0))
        port = closed.getsockname()[1]
        closed.close()
        self.console.load_settings = lambda: {'host': '127.0.0.1', 'klog_port': port, 'ctl_port': 2}
        self.assertEqual(self.new_boot(), 2)
        self.assertIsNotNone(guard.load()['dropped'])


class DropRetryTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.sleeps = []
        for context in (
            patch.object(guard, 'STATE', Path(self.scratch.name) / 'state.json'),
            patch.object(guard.time, 'sleep', self.sleeps.append),
        ):
            context.start()
            self.addCleanup(context.stop)
        guard.new_boot()

    def test_a_console_that_answers_again_is_not_recorded(self):
        replies = [SystemExit('did not answer'), SystemExit('did not answer'), 'ok ps5vkctl 1']
        ping = Mock(side_effect=replies)
        self.assertFalse(guard.record_drop_unless_answering('ps5vkctl timed out', ping))
        self.assertEqual(ping.call_count, 3)
        self.assertEqual(self.sleeps, [guard.DROP_INTERVAL] * 2)
        self.assertIsNone(guard.load()['dropped'])
        guard.check_launch_allowed()

    def test_a_silent_console_is_recorded_after_about_a_minute(self):
        ping = Mock(side_effect=SystemExit('did not answer'))
        self.assertTrue(guard.record_drop_unless_answering('ps5vkctl timed out', ping))
        self.assertEqual(ping.call_count, guard.DROP_RETRIES)
        self.assertGreaterEqual(sum(self.sleeps), 55)
        self.assertEqual(guard.load()['dropped']['why'], 'ps5vkctl timed out')
        with self.assertRaises(SystemExit):
            guard.check_launch_allowed()

    def test_an_os_error_counts_as_silence(self):
        ping = Mock(side_effect=[OSError('No route to host'), 'ok'])
        self.assertFalse(guard.record_drop_unless_answering('x', ping, retries=3, interval=1))
        self.assertEqual(ping.call_count, 2)


class FakeFTP:
    """The console's FTP server as the tools use it: RETR of a dict of files, 550 for anything else."""

    def __init__(self, files):
        self.files = files
        self.asked = []

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def retrbinary(self, command, callback, blocksize=8192):
        path = command.removeprefix('RETR ')
        self.asked.append(path)
        if path not in self.files:
            raise ftplib.error_perm('550 No such file or directory')
        callback(self.files[path])


class RunnerHelperTests(unittest.TestCase):
    def test_installed_profiles_are_the_listed_folders(self):
        listing = ['drwxrwxrwx 1 0 0 512 Oct 14 15:46 .',
                   'drwxrwxrwx 1 0 0 512 Oct 14 15:46 ..',
                   'drwxrwxrwx 1 0 0 512 Oct 14 15:46 mihawk-os',
                   'drwxrwxrwx 1 0 0 512 Oct 14 15:46 1',
                   '-rw-rw-rw- 1 0 0 12 Oct 14 15:46 notes.txt',
                   'drwxrwxrwx 1 0 0 512 Oct 14 15:46 .hidden',
                   'drwxrwxrwx 1 0 0 512 Oct 14 15:46 Some Game']
        self.assertEqual(run_console.installed_profiles(listing), {'mihawk-os', '1', 'Some Game'})
        self.assertEqual(run_console.installed_profiles([]), set())

    def test_session_logs_status_is_the_last_report(self):
        lines = ['[MihawkOS] Wine: session logs status=2 removed=0',
                 '[MihawkOS] Wine: session logs status=0 removed=3 title=0 wine=0']
        self.assertEqual(run_console.session_logs_status(lines), 0)
        self.assertEqual(run_console.session_logs_status(lines[:1]), 2)
        self.assertIsNone(run_console.session_logs_status(['[MihawkOS] Wine: load status=0']))

    def test_child_logs_come_only_from_this_session(self):
        sessions = run_console.SESSIONS
        ftp = FakeFTP({f'{sessions}/0/wine-child-7.log': b'wine-child: entry\n',
                       f'{sessions}/1/wine-child-9.log': b'an older session of the same pid\n'})
        owner = 'wine-ps5: payload rendezvous native=7\nwine-ps5: child ready native=9\n'
        with tempfile.TemporaryDirectory() as directory, patch.object(run_console, 'connect', lambda *a: ftp):
            paths, missing = run_console.fetch_child_logs({}, owner, Path(directory) / 'run.log')
            self.assertEqual([p.name for p in paths], ['run-wine-child-7.log'])
            self.assertEqual(missing, [9])
            self.assertFalse((Path(directory) / 'run-wine-child-9.log').exists())
        self.assertEqual(ftp.asked, [f'{sessions}/0/wine-child-7.log', f'{sessions}/0/wine-child-9.log'])

    def test_sessions_root_is_the_titles(self):
        header = ROOT / 'src/pw_log_sessions.h'
        if not header.is_file():
            self.skipTest('src/pw_log_sessions.h is not in this tree yet')
        root = re.search(r'#define\s+PW_LOG_SESSIONS_ROOT\s+"([^"]+)"', header.read_text())
        self.assertEqual(root.group(1), run_console.SESSIONS)


class VerifyTitleTests(unittest.TestCase):
    def test_self_mode_is_turned_off(self):
        ftp = SimpleNamespace(sendcmd=Mock(side_effect=['226 SELF transfer mode enabled',
                                                        '226 SELF transfer mode disabled']))
        verify_title.self_off(ftp)
        self.assertEqual(ftp.sendcmd.call_count, 2)
        ftp = SimpleNamespace(sendcmd=Mock(return_value='226 SELF transfer mode enabled'))
        with self.assertRaisesRegex(SystemExit, 'SELF mode did not turn off'):
            verify_title.self_off(ftp)

    def test_every_file_is_compared_by_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory) / 'PPSA99170'
            (folder / 'sce_module').mkdir(parents=True)
            for name, data in (('eboot.bin', b'eboot'), ('sce_module/libc.prx', b'libc'), ('wine-child.bin', b'w')):
                (folder / name).write_bytes(data)
            base = '/data/homebrew/PPSA99170'
            ftp = FakeFTP({f'{base}/eboot.bin': b'eboot', f'{base}/sce_module/libc.prx': b'libc, one block lost'})
            bad, digest = verify_title.title_mismatches(ftp, folder)
            self.assertEqual(bad, ['sce_module/libc.prx', 'wine-child.bin'])
            ftp.files[f'{base}/sce_module/libc.prx'] = b'libc'
            ftp.files[f'{base}/wine-child.bin'] = b'w'
            self.assertEqual(verify_title.title_mismatches(ftp, folder), ([], digest))
            (folder / 'eboot.bin').write_bytes(b'eboot2')
            self.assertNotEqual(verify_title.title_mismatches(ftp, folder)[1], digest)

    def test_the_runtime_manifest_is_this_builds(self):
        staged = {'runtime/ge11-7/a.so': 'aa', 'run/proton-payload-child.elf': 'cc',
                  'prefixes/probe/drive_c/wine-probe.exe': 'pp'}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'manifest.json'
            path.write_text(json.dumps(staged))
            def problems(console):
                files = {} if console is None else {verify_title.RUNTIME_MANIFEST: json.dumps(console).encode()}
                return verify_title.runtime_problems(FakeFTP(files), path)
            self.assertEqual(problems(staged), [])
            self.assertEqual(problems({k: v for k, v in staged.items() if k.startswith('r')}), [])
            self.assertEqual(problems(dict(staged, **{'runtime/ge11-7/a.so': 'old'})), ['runtime runtime/ge11-7/a.so differs'])
            self.assertEqual(problems({'runtime/ge11-7/a.so': 'aa'}), ['runtime run/proton-payload-child.elf missing'])
            self.assertEqual(problems(None), ['runtime: no deploy manifest on the console'])


class BatteryRefusalTests(unittest.TestCase):
    def test_the_battery_stops_before_a_refused_case(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for folder in ('evidence', 'klog', '.deps'):
                (root / folder).mkdir()
            (root / '.deps/console-guard.json').write_text(json.dumps(
                {'dropped': None, 'storage': {'run': 'storage-run-01', 'at': 0, 'lines': PROF_08[:1]}}))
            run = Mock()
            with patch.object(battery, 'ROOT', root), \
                 patch.object(sys, 'argv', ['run-battery.py', '--name', 'host-test', '--gate']), \
                 patch.object(battery.subprocess, 'check_output', return_value='host-revision'), \
                 patch.object(battery.subprocess, 'run', run), \
                 patch('sys.stdout', io.StringIO()):
                status = battery.main()
            record = json.loads((root / 'evidence/host-test.json').read_text())
        self.assertEqual(status, 1)
        run.assert_not_called()
        self.assertIn('storage damage', record['stop_reason'])
        self.assertFalse(record['passed'])


if __name__ == '__main__':
    unittest.main()
