#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The WebUI's words in every language (webui/i18n.js): each English string the page can show has its text in each
language the guided setup lists (src/pw_setup.c), with the same {0}, {1} values in it, and no language holds a string
the page no longer shows; and each language's flag is one the page serves. The strings are the page's own text,
placeholders and labels (webui/index.html), every t('...') in webui/app.js, and the server's complete messages
(src/pw_webui.cpp, the controller and button labels).

  python3 tests/test_webui_i18n.py           the check
  python3 tests/test_webui_i18n.py --list    the strings, as JSON (what a translator is given)
"""
import json
import re
import sys
import unittest
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PREFIX = 'window.MIHAWK_I18N = '


class _Text(HTMLParser):
    def __init__(self):
        super().__init__()
        self.skip, self.found = 0, []

    def handle_starttag(self, tag, attrs):
        if tag in ('script', 'style', 'title', 'pre', 'head'):
            self.skip += 1
        for name, value in attrs:
            if name in ('placeholder', 'aria-label') and value:
                self.found.append(value)

    def handle_endtag(self, tag):
        if tag in ('script', 'style', 'title', 'pre', 'head'):
            self.skip -= 1

    def handle_data(self, data):
        text = ' '.join(data.split())
        if not self.skip and re.search('[A-Za-z]', text):
            self.found.append(text)


def _unescape(text):
    return re.sub(r'\\(.)', r'\1', text)


def strings():
    parser = _Text()
    parser.feed((ROOT / 'webui/index.html').read_text())
    found = parser.found
    found += [_unescape(m) for m in re.findall(r"\bt\('((?:[^'\\]|\\.)*)'", (ROOT / 'webui/app.js').read_text())]
    server = (ROOT / 'src/pw_webui.cpp').read_text()
    found += [_unescape(m) for m in re.findall(r'fail\(\d+, "((?:[^"\\]|\\.)*)"\)', server)]
    found += [_unescape(m) for m in re.findall(r'why = "((?:[^"\\]|\\.)*)"[,;]', server)]
    found += re.findall(r'\{"label", "([^"]+)"\}', server)
    found += re.findall(r'\{"[a-z0-9]+", "([^"]+)", [A-Z_0-9]+\}', (ROOT / 'src/pw_controls.c').read_text())
    names = re.search(r'names\[PW_CONTROLLER_KINDS\] = \{([^}]*)\}', (ROOT / 'src/pw_controller.c').read_text())
    found += re.findall(r'"([^"]+)"', names.group(1))
    return sorted(set(found))


def languages():
    return re.findall(r'\{"([a-z]{2}-[A-Z]{2})", "([^"]+)"', (ROOT / 'src/pw_setup.c').read_text())


def page_text():
    source = (ROOT / 'webui/i18n.js').read_text()
    start = source.index(PREFIX) + len(PREFIX)
    return json.loads(source[start:source.rindex(';')])


class WebuiWords(unittest.TestCase):
    def test_every_language_has_every_string(self):
        wanted, text = strings(), page_text()
        self.assertGreater(len(wanted), 100)
        self.assertEqual([one['tag'] for one in text['languages']], [tag for tag, _ in languages()])
        for one in text['languages']:   # its flag is one the page serves (src/pw_webui.cpp's asset list)
            self.assertIn(f'"/flags/{one["flag"]}.svg"', (ROOT / 'src/pw_webui.cpp').read_text(), one['tag'])
            self.assertTrue((ROOT / 'ps5/assets/flags' / f'{one["flag"]}.svg').is_file(), one['tag'])
        for tag, words in text['text'].items():
            with self.subTest(language=tag):
                self.assertEqual(sorted(set(wanted) - set(words)), [], 'missing')
                self.assertEqual(sorted(set(words) - set(wanted)), [], 'no longer shown')
                for english, said in words.items():
                    self.assertEqual(sorted(re.findall(r'\{\d\}', english)), sorted(re.findall(r'\{\d\}', said)), english)
                    self.assertTrue(said.strip(), english)
        self.assertEqual(sorted(text['text']), sorted(tag for tag, _ in languages() if tag != 'en-US'))


if __name__ == '__main__':
    if '--list' in sys.argv:
        print(json.dumps(strings(), ensure_ascii=False, indent=1))
    else:
        unittest.main()
