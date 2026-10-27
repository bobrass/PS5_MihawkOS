# SPDX-License-Identifier: GPL-3.0-or-later
"""What the native File Explorer must show on Home, and on a folder (Documents), where its pixels do not depend on the
wallpaper or on the files:
the prototype's own sprites (themes/windows11/explorer, tools/measure-explorer-sprites.mjs) composed where the
prototype has them over the solid surfaces they sit on, in the client area of the prototype's 1100 x 700 window
(1098 x 698 inside its border). Wine patch 1150 draws the same sprites, so a capture of the window on Home equals this
image wherever the mask is set: the header (#f9f9f9), the navigation pane, the Home page down to its Recent rows and
the status bar's buttons (on #ffffff), and the title bar's fully opaque sprite pixels (the rest is Mica Alt). On a folder
the location's names (the tab's title, the crumbs, the search box's text) and its files are not compared; the command
bar with Filter, the navigation pane with the folder selected and the details view's column header are.

Used by tools/run-desktop-session.py (desktop-session's verdict) and to compare a console capture region by region.
"""
import functools
import gzip
import importlib
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
WIDTH, HEIGHT = 1098, 698
SURFACES = {'light': {'header': (249, 249, 249), 'body': (255, 255, 255), 'divider': (234, 234, 234)},
            'dark': {'header': (44, 44, 44), 'body': (28, 28, 28), 'divider': (61, 61, 61)}}
TITLE = ['tab-home', 'tab-add', 'cap-min', 'cap-max', 'cap-close']
HEADER = ['back-disabled', 'forward-disabled', 'up-disabled', 'refresh', 'address', 'crumb-icon-home', 'crumb-chev',
          'crumb-home', 'search', 'search-text-home', 'search-glyph', 'sep-1', 'sep-2', 'sep-3',
          *[f'cmd-{k}-disabled' for k in ('new', 'cut', 'copy', 'paste', 'rename', 'share', 'delete')],
          'cmd-sort', 'cmd-view-details', 'cmd-more', 'cmd-details']
NAV = ['nav-selected', *[f'nav-{k}' for k in ('home', 'gallery', 'desktop', 'downloads', 'documents', 'pictures', 'music',
                                              'videos', 'root', 'network')], 'nav-sep']
PAGE = ['head-qa', 'head-fav', 'head-recent', *[f'qa-{k}' for k in ('desktop', 'downloads', 'documents', 'pictures', 'music',
                                                                     'videos')], 'fav-note', 'recent-head']
STATUS = ['status-details-disabled', 'status-large-disabled']
RECENT_ROWS_Y = 144 + 358  # the Home page's first Recent row, client y (tests/test_pw_explorer_layout.c)
FOLDER_TITLE = ['tab', 'tab-icon-documents', 'tab-add', 'cap-min', 'cap-max', 'cap-close']
FOLDER_HEADER = ['back-disabled', 'forward-disabled', 'up', 'refresh', 'address', 'search', 'search-glyph', 'sep-1', 'sep-2',
                 'sep-3-folder', 'cmd-new', *[f'cmd-{k}-disabled' for k in ('cut', 'copy', 'paste', 'rename', 'share', 'delete')],
                 'cmd-sort', 'cmd-view-details', 'cmd-filter', 'cmd-more-folder', 'cmd-details']
FOLDER_COLUMNS = ['dv-name-plain', 'dv-modified', 'dv-type', 'dv-size']
NAV_ROW = {'home': 0, 'documents': 293 - 148}  # the selected plate's offset from Home's row
VIEWS = ('large', 'small', 'list')  # Documents in an icon view: its first item (a folder) is compared, not its name
TABS_TITLE = [('tab-icon-documents', 0), ('tab-home', 240), ('tab-add', 240), ('cap-min', 0), ('cap-max', 0), ('cap-close', 0)]


@functools.lru_cache(maxsize=2)
def sprites(theme):
    """{name: (x, y, premultiplied RGB float array h x w x 3, alpha h x w)} in client coordinates."""
    premultiplied = importlib.import_module('build-shell-sprites').premultiplied
    out = {}
    for name, s in json.loads(gzip.decompress((ROOT / f'themes/windows11/explorer/explorer-{theme}.json.gz').read_bytes()))['sprites'].items():
        words = np.array(premultiplied(s), dtype=np.uint32).reshape(s['h'], s['w'])
        rgb = np.stack([(words >> 16) & 255, (words >> 8) & 255, words & 255], axis=-1).astype(np.float64)
        out[name] = (s['x'], s['y'], rgb, ((words >> 24) & 255).astype(np.float64))
    return out


def home(theme, state='home'):
    """(expected RGB uint8 HEIGHT x WIDTH x 3, mask bool HEIGHT x WIDTH) for the window on Home, on Documents, on
    Documents in large icons, small icons or list (state 'large', 'small', 'list'), on Documents with the details pane
    and Filter's Documents on (state 'filter': nothing listed, the prototype's note) or with its first item, a folder,
    selected (state 'pane'), on a second tab on Home beside an unselected Documents tab (state 'tabs'), or on that tab
    gone up from the navigation pane's Documents (state 'parent': Wine's Documents there is the shell's own item under
    the Desktop, so up is the Desktop, its row selected, back enabled and up disabled)."""
    surface, s = SURFACES[theme], sprites(theme)
    image = np.zeros((HEIGHT, WIDTH, 3)); mask = np.zeros((HEIGHT, WIDTH), bool)
    image[48:143] = surface['header']; image[143] = surface['divider']; image[144:] = surface['body']

    def put(name, dy=0, opaque_only=False, top=0, dx=0):
        x, y, rgb, alpha = s[name]; y += dy + top; x += dx
        rgb, alpha = rgb[top:], alpha[top:]  # top: rows clipped above (the sort glyph over the header's top)
        cut = max(0, -x)  # the tab's outward corner reaches into the border, outside the client area
        rgb, alpha, x = rgb[:, cut:], alpha[:, cut:], x + cut
        h, w = alpha.shape
        region = image[y:y + h, x:x + w]
        region[...] = rgb + region * (255 - alpha[..., None]) / 255
        if opaque_only: mask[y:y + h, x:x + w] |= alpha == 255
    if state in VIEWS:
        for name in FOLDER_TITLE: put(name, opaque_only=True)
        mask[16:42, 41:209] = False           # the tab's title
        header = [f'cmd-view-{state}' if name == 'cmd-view-details' else name for name in FOLDER_HEADER]
        for name in header + ['status-details-plain', 'status-large-checked' if state == 'large' else 'status-large']: put(name)
        put('nav-selected', dy=NAV_ROW['documents'])
        for name in NAV[1:]: put(name)
        put('nav-sep', dy=429 - 220)
        # The first tile's icon: large 10,6 into a tile 8 px down, small 8,8 into one 8 px down, list 8,8 into one 4 px
        # down (the sprites were captured in large and small icons).
        icon, dy = ('icon-folder-96', 0) if state == 'large' else ('icon-folder-16', -4 if state == 'list' else 0)
        put(icon, dy=dy)
        x, y, rgb, alpha = s[icon]
        mask[y + dy:y + dy + alpha.shape[0], x:x + alpha.shape[1]] = True
        mask[48:144] = True
        mask[57:87, 157:797] = False          # the crumbs
        mask[57:87, 807:1053] = False         # "Search Documents"
        mask[144:672, :165] = True
        mask[672:, 1000:] = True
    elif state == 'parent':
        for name in ['tab', 'tab-icon-desktop', 'tab-add', 'cap-min', 'cap-max', 'cap-close']: put(name, opaque_only=True)
        mask[16:42, 41:209] = False           # the tab's title
        header = [{'back-disabled': 'back', 'up': 'up-disabled'}.get(n, n) for n in FOLDER_HEADER]
        for name in header + FOLDER_COLUMNS + ['status-details', 'status-large']: put(name)
        put('nav-selected', dy=229 - 148)     # Desktop's row
        for name in NAV[1:]: put(name)
        put('dv-sort-up', top=1)
        put('nav-sep', dy=429 - 220)
        mask[48:144] = True
        mask[57:87, 157:797] = False          # the crumbs
        mask[57:87, 807:1053] = False         # the search box's text
        mask[144:672, :165] = True
        mask[144:178, 165:] = True
        mask[144:660, 165:177] = mask[144:660, 1090:] = True
        # The Desktop lists the user's own shortcuts: past 13 or so items its vertical scrollbar takes this column, over
        # the column header too (19 items on 2026-10-24 failed the gate on these 141 pixels).
        mask[144:672, 1073:1090] = False
        mask[672:, 1000:] = True
    elif state in ('filter', 'pane'):
        for name in FOLDER_TITLE: put(name, opaque_only=True)
        mask[16:42, 41:209] = False           # the tab's title
        swap = {'cmd-details': 'cmd-details-checked'}
        if state == 'filter': swap['cmd-filter'] = 'cmd-filter-checked'
        else: swap.update({f'cmd-{k}-disabled': f'cmd-{k}' for k in ('cut', 'copy', 'rename', 'delete')})
        for name in [swap.get(n, n) for n in FOLDER_HEADER] + FOLDER_COLUMNS + ['status-details', 'status-large']: put(name)
        put('nav-selected', dy=NAV_ROW['documents'])
        for name in NAV[1:]: put(name)
        put('dv-sort-up', top=1)
        put('nav-sep', dy=429 - 220)
        mask[48:144] = True
        mask[57:87, 157:797] = False          # the crumbs
        mask[57:87, 807:1053] = False         # "Search Documents"
        mask[144:672, :165] = True
        mask[144:178, 165:818] = True         # the column header, in the items area the pane narrows to 653 px
        mask[144:660, 165:177] = True
        image[144:672, 818:] = surface['body']  # the pane covers the columns past the items area (700 px in 633)
        if state == 'filter':
            put('note-filtered', dx=(700 - 913) // 2)  # centred in the grid, its 700 px of columns wider than the 633 px area
            for name in ('dp-divider', 'dp-loc-documents', 'dp-hint'): put(name)
            mask[178:226, 177:806] = True     # the note
            mask[144:672, 818:] = True        # the pane, but the location's name (GDI's text)
            mask[272:296, 818:] = False
        else:
            for name in ('dp-divider', 'dp-icon-folder', 'dp-title', 'dp-key-contains', 'dp-key-modified', 'dp-key-path'): put(name)
            mask[144:672, 818] = True         # the pane's divider, its icon, Details and the keys (the values are text)
            mask[164:260, 910:1007] = True
            mask[330:371, 835:1082] = True
            mask[371:451, 835:911] = True
        mask[672:, 1000:] = True
    elif state == 'documents':
        for name in FOLDER_TITLE: put(name, opaque_only=True)
        mask[16:42, 41:209] = False           # the tab's title (the folder's name, GDI's text)
        for name in FOLDER_HEADER + FOLDER_COLUMNS + ['status-details', 'status-large']: put(name)
        put('nav-selected', dy=NAV_ROW[state])
        for name in NAV[1:]: put(name)        # the rows over the plate
        put('dv-sort-up', top=1)
        put('nav-sep', dy=429 - 220)
        mask[48:144] = True
        mask[57:87, 157:797] = False          # the crumbs (the folder's path)
        mask[57:87, 807:1053] = False         # "Search Documents" (GDI's text)
        mask[144:672, :165] = True
        mask[144:178, 165:] = True            # the column header and the 2 px under it
        mask[144:660, 165:177] = mask[144:660, 1090:] = True  # the items area's padding
        mask[672:, 1000:] = True
    else:
        for name, dx in TABS_TITLE if state == 'tabs' else [(name, 0) for name in TITLE]: put(name, opaque_only=True, dx=dx)
        for name in HEADER + NAV + PAGE + STATUS: put(name)
        put('nav-sep', dy=429 - 220)
        mask[48:144] = True                       # the header and its divider
        mask[144:672, :165] = True                # the navigation pane
        mask[144:RECENT_ROWS_Y, 165:] = True      # Home above its Recent rows (which list the prefix's own files)
        mask[672:, 1000:] = True                  # the status bar's view buttons (the item count is text)
    mask[HEIGHT - 8:, WIDTH - 8:] = False     # the rounded corner, where the border and the wallpaper show
    return np.clip(np.rint(image), 0, 255).astype(np.uint8), mask


def compare(capture, left, top, theme, pointer=None, state='home'):
    """Mismatches of a capture (RGB uint8 array) whose window client starts at left, top: counts and the worst. The
    pointer's 32 x 32 sprite at the screen point pointer (x, y) is left out."""
    expected, mask = home(theme, state)
    if pointer:
        x, y = pointer[0] - left, pointer[1] - top
        mask[max(0, y - 1):max(0, y + 31), max(0, x - 1):max(0, x + 31)] = False
    got = capture[top:top + HEIGHT, left:left + WIDTH].astype(int)
    if got.shape[:2] != (HEIGHT, WIDTH): return {'compared': 0, 'wrong': -1, 'worst': 255}
    error = np.abs(got - expected.astype(int)).max(axis=-1)
    wrong = mask & (error > 2)
    first = np.argwhere(wrong)
    return {'compared': int(mask.sum()), 'wrong': int(wrong.sum()), 'worst': int(error[mask].max()),
            'within_1': round(float((error[mask] <= 1).mean()), 4),
            'first_wrong': [int(first[0][1]), int(first[0][0])] if len(first) else None}
