# SPDX-License-Identifier: GPL-3.0-or-later
"""The WebUI over HTTP (src/pw_webui_http.cpp), on this PC: the server tools/run-webui-host.sh builds, over a folder of
its own, on a free port. Only the console's own address is answered; a write needs the token; a body is bounded; an
upload arrives in pieces, resumes at the console's size and is whole only at its last byte (a resumed piece once landed
at the file's start: 2026-10-26); the page's files carry the security headers."""
import io
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class WebuiHttp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        built = subprocess.run(['bash', str(ROOT / 'tools/run-webui-host.sh')], env=dict(os.environ, WEBUI_HOST_BUILD_ONLY='1'),
                               capture_output=True, text=True, check=True)
        binary = built.stdout.strip().splitlines()[-1]
        cls.folder = tempfile.TemporaryDirectory()
        base = Path(cls.folder.name)
        cls.data, title = base / 'data', base / 'title'
        (cls.data / 'prefixes/mihawk/drive_c/Games').mkdir(parents=True)
        (cls.data / 'logs').mkdir()
        (title / 'webui').mkdir(parents=True)
        (title / 'webui/index.html').write_text('<!doctype html><title>t</title>')
        cls.port = free_port()
        cls.server = subprocess.Popen([binary, str(cls.data), str(title), str(cls.port)], stdout=subprocess.PIPE, text=True)
        assert 'webui-host: http' in cls.server.stdout.readline()
        cls.base = f'http://127.0.0.1:{cls.port}'
        cls.token = cls.call('GET', '/api/status')[1]['token']

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()
        cls.server.wait(10)
        cls.server.stdout.close()
        cls.folder.cleanup()

    @classmethod
    def call(cls, method, path, body=None, headers=None, raw=False):
        request = urllib.request.Request(cls.base + path, data=body, method=method, headers=headers or {})
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                data = response.read()
                return response.status, (data if raw else json.loads(data)), dict(response.headers)
        except urllib.error.HTTPError as error:
            with error:
                data = error.read()
                return error.code, json.loads(data or b'{}'), dict(error.headers)

    def write(self, method, path, body=b'', token=None):
        return self.call(method, path, body, {'X-Mihawk-Token': token if token is not None else self.token})

    def test_only_the_consoles_address(self):
        for host in ('evil.example:%d' % self.port, 'localhost:%d' % self.port, '127.0.0.1:1'):
            self.assertEqual(self.call('GET', '/api/status', headers={'Host': host})[0], 403, host)
        status = self.call('GET', '/api/status', headers={'Origin': 'http://evil.example'})[0]
        self.assertEqual(status, 403)

    def test_writes_need_the_token(self):
        body = json.dumps({'game_mode': False}).encode()
        self.assertEqual(self.write('POST', '/api/settings', body, token='')[0], 403)
        self.assertEqual(self.write('POST', '/api/settings', body, token='0' * 32)[0], 403)
        self.assertEqual(self.write('POST', '/api/settings', body)[0], 200)
        self.assertEqual((self.data / 'game-mode.txt').read_text(), 'game-mode off\n')

    def test_a_body_is_bounded(self):
        self.assertEqual(self.write('POST', '/api/settings', b' ' * (65 * 1024))[0], 413)

    def test_upload_in_pieces_and_resume(self):
        content = os.urandom(3_000_000)
        query = f'/api/upload?prefix=mihawk&path=Games/Http%20Test/data/blob.bin&size={len(content)}'
        status, reply, _ = self.write('PUT', query + '&offset=0', content[:1_000_000])
        self.assertEqual((status, reply), (200, {'have': 1_000_000, 'done': False}))
        status, reply, _ = self.call('GET', '/api/upload/status?prefix=mihawk&path=Games/Http%20Test/data/blob.bin')
        self.assertEqual(reply, {'done': False, 'have': 1_000_000})
        self.assertEqual(self.write('PUT', query + '&offset=5', content[5:])[0], 409)        # resume at the console's size
        self.assertEqual(self.write('PUT', query + '&offset=1000000', content[1_000_000:])[1], {'have': 3_000_000, 'done': True})
        target = self.data / 'prefixes/mihawk/drive_c/Games/Http Test/data/blob.bin'
        self.assertEqual(target.read_bytes(), content)
        self.assertFalse(target.with_name('blob.bin.part').exists())
        for path in ('Windows/x.dll', 'Games/x', 'Games/../x/y', 'Games/New/.hidden'):
            status = self.write('PUT', f'/api/upload?prefix=mihawk&path={path}&offset=0&size=1', b'x')[0]
            self.assertEqual(status, 400, path)
        self.assertEqual(self.write('PUT', query + '&offset=0', content[:10], token='')[0], 403)

    def test_a_folder_as_a_zip_and_a_save_restored(self):
        saves = self.data / 'prefixes/mihawk/drive_c/users/Mihawk/Saved Games'
        (saves / 'deep').mkdir(parents=True)
        files = {'slot1.sav': os.urandom(70_000), 'deep/slot2.sav': b'second', 'empty.cfg': b''}
        for name, content in files.items():
            (saves / name).write_bytes(content)
        request = urllib.request.Request(self.base + '/api/zip?prefix=mihawk&path=users/Mihawk/Saved%20Games')
        with urllib.request.urlopen(request, timeout=10) as response:
            self.assertEqual(response.headers['Content-Type'], 'application/zip')
            self.assertIn('Saved Games.zip', response.headers['Content-Disposition'])
            archive = zipfile.ZipFile(io.BytesIO(response.read()))
        self.assertIsNone(archive.testzip())   # every CRC right
        self.assertEqual({info.filename: archive.read(info) for info in archive.infolist()},
                         {f'users/Mihawk/Saved Games/{name}': content for name, content in files.items()})
        self.assertEqual(self.call('GET', '/api/zip?prefix=mihawk&path=users')[0], 404)
        self.assertEqual(self.call('GET', '/api/zip?prefix=mihawk&path=users/../Games')[0], 404)
        # restored: a file sent back into the user's folder
        query = '/api/upload?prefix=mihawk&path=users/Mihawk/Saved%20Games/slot3.sav&size=4&offset=0'
        self.assertEqual(self.write('PUT', query, b'back')[1], {'have': 4, 'done': True})
        self.assertEqual((saves / 'slot3.sav').read_bytes(), b'back')

    def test_a_bug_report(self):
        (self.data / 'logs/sessions/0').mkdir(parents=True, exist_ok=True)
        (self.data / 'logs/title-last.log').write_text('reached 192.0.2.9:6767\n' + 'x' * 600_000)
        (self.data / 'logs/sessions/0/wine.log').write_text('wine ok\n')
        request = urllib.request.Request(self.base + '/api/report')
        with urllib.request.urlopen(request, timeout=10) as response:
            self.assertIn('mihawkos-report.zip', response.headers['Content-Disposition'])
            archive = zipfile.ZipFile(io.BytesIO(response.read()))
        self.assertIsNone(archive.testzip())
        names = set(archive.namelist())
        self.assertEqual(names, {'about.txt', 'logs/title-last.log', 'logs/sessions/0/wine.log'})
        title_log = archive.read('logs/title-last.log').decode()
        self.assertTrue(title_log.startswith('[... the start is left out'))   # 512 KiB kept: the address was in the cut
        self.assertNotIn('192.0.2', title_log)
        self.assertEqual(archive.read('logs/sessions/0/wine.log'), b'wine ok\n')
        self.assertIn('settings {', archive.read('about.txt').decode())

    def test_no_file_is_sent_by_descriptor(self):
        # MHD sends a descriptor response with sendfile, which on the console stops after 33 KB (2026-10-26): every
        # file goes through a read callback (src/pw_webui_http.cpp respond_file)
        self.assertNotIn('MHD_create_response_from_fd', (ROOT / 'src/pw_webui_http.cpp').read_text())
        (self.data / 'prefixes/mihawk/drive_c/users/Mihawk/Big').mkdir(parents=True, exist_ok=True)
        big = os.urandom(300_000)
        (self.data / 'prefixes/mihawk/drive_c/users/Mihawk/Big/big.bin').write_bytes(big)
        status, body, _ = self.call('GET', '/api/file?prefix=mihawk&path=users/Mihawk/Big/big.bin', raw=True)
        self.assertEqual((status, body), (200, big))

    def test_page_headers(self):
        status, body, headers = self.call('GET', '/', raw=True)
        self.assertEqual(status, 200)
        self.assertIn("default-src 'self'", headers['Content-Security-Policy'])
        self.assertEqual(headers['X-Content-Type-Options'], 'nosniff')
        self.assertEqual(self.call('GET', '/app.js')[0], 404)   # listed, but not in this title folder


if __name__ == '__main__':
    sys.exit(0 if unittest.main(exit=False, verbosity=1).result.wasSuccessful() else 1)
