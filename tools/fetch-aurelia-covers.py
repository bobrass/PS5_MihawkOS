#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch the Aurelia library's cover art onto the console, once per app id.

The session screen (native/proton_loading.cpp) draws 2:3 posters and reads the picture at
/data/ps5proton/covers/<app id>.jpg. The headless client's list --json carries the CDN URLs (assets.capsule:
library_600x900.jpg); this tool downloads them on the PC -- the console's own link is slow and its title's TLS
client is for the runtime install -- and uploads them over the console's FTP, idempotently.

  tools/fetch-aurelia-covers.py [--appid APPID]... [--force]

The library's app ids come from /data/ps5proton/aurelia-library.txt (one `app_id<TAB>name...` line per game). A
picture already on the console is kept unless --force. The images are the store's own artwork, fetched for this
user's console: nothing of them is committed to the repository or logged.
"""
import argparse
import io
import json
import sys
import urllib.error
import urllib.request
from ftplib import FTP
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

DATA = '/data/ps5proton'
COVERS = f'{DATA}/covers'
# The client's list --json names these in assets.capsule / assets.header; the capsule is the 2:3 poster the
# shelf uses, the header is the fallback when a store entry has no capsule.
URLS = ('https://cdn.cloudflare.steamstatic.com/steam/apps/{}/library_600x900.jpg',
        'https://cdn.cloudflare.steamstatic.com/steam/apps/{}/header.jpg',
        'https://cdn.cloudflare.steamstatic.com/steam/apps/{}/library_hero.jpg')
# A store entry whose CDN capsule is gone still has a header or capsule in the store API (two such app ids in the
# 2026-10-22 library: one only had library_hero.jpg, one only the store API's header).
API = 'https://store.steampowered.com/api/appdetails?appids={}&filters=basic'
LIMIT = 4 << 20


def read_library(ftp):
    app_ids = []
    raw = io.BytesIO()
    try:
        ftp.retrbinary(f'RETR {DATA}/aurelia-library.txt', raw.write)
    except Exception as error:
        raise SystemExit(f'no {DATA}/aurelia-library.txt on the console ({error}); run a list session first') from None
    for line in raw.getvalue().decode('utf-8', 'replace').splitlines():
        field = line.split('\t', 1)[0]
        if field.isdigit():
            app_ids.append(int(field))
    return app_ids


def ensure_directory(ftp):
    ftp.cwd('/')
    ftp.cwd(DATA)
    try:
        ftp.mkd('covers')
    except Exception:
        pass


def console_names(ftp):
    # The console's FTP server answers SIZE for a missing name and refuses NLST, so its LIST is the reliable check.
    try:
        lines = []
        ftp.retrlines(f'LIST {COVERS}', lines.append)
        return {line.split()[-1] for line in lines if line}
    except Exception:
        return set()


def download(url):
    with urllib.request.urlopen(url, timeout=30) as response:
        data = response.read(LIMIT + 1)
        if response.status != 200 or len(data) > LIMIT:
            raise OSError(f'{response.status} {len(data)} bytes')
        if not data.startswith(b'\xff\xd8\xff'):
            raise OSError('not a JPEG')
        return data


def fetch(app_id):
    last = None
    for pattern in URLS:
        try:
            return download(pattern.format(app_id))
        except (urllib.error.URLError, OSError) as error:
            last = str(error)
    try:
        with urllib.request.urlopen(API.format(app_id), timeout=30) as response:
            data = json.load(response).get(str(app_id), {}).get('data', {})
        for key in ('header_image', 'capsule_imagev5', 'capsule_image'):
            if data.get(key):
                try:
                    return download(data[key])
                except (urllib.error.URLError, OSError) as error:
                    last = str(error)
    except (urllib.error.URLError, OSError, ValueError) as error:
        last = str(error)
    raise OSError(last or 'no URL answered')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--appid', type=int, action='append', default=[], help='one app id (repeatable); default: the library')
    parser.add_argument('--force', action='store_true', help='fetch again even when the console already has a picture')
    args = parser.parse_args()
    settings = ps5_console.load_settings()
    fetched = kept = 0
    failed = []
    with FTP() as ftp:
        ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
        ftp.login(settings['ftp_user'], settings['ftp_password'] or 'codex')
        app_ids = args.appid or read_library(ftp)
        ensure_directory(ftp)
        present = console_names(ftp)
        for app_id in app_ids:
            path = f'{COVERS}/{app_id}.jpg'
            if not args.force and f'{app_id}.jpg' in present:
                kept += 1
                continue
            try:
                data = fetch(app_id)
                ftp.storbinary(f'STOR {path}', io.BytesIO(data))
                fetched += 1
                print(f'cover {app_id}: {len(data)} bytes')
            except Exception as error:
                failed.append(app_id)
                print(f'cover {app_id}: {error}', file=sys.stderr)
    print(f'covers: {fetched} fetched, {kept} already there, {len(failed)} failed'
          + (f' (app ids {", ".join(str(i) for i in failed)})' if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
