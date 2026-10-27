# The README's art and badges

The README's pictures are SVG drawn for this project in its Frutiger Aero look, so that GitHub shows them as drawn:
GitHub keeps Markdown and a few HTML tags, and drops every style and script. Each picture comes in a day and a night
version. The README picks one with `<picture><source media="(prefers-color-scheme: dark)">`, which GitHub matches to the
reader's theme.

| Files | What they are |
| --- | --- |
| `docs/media/readme/banner-{day,night}.svg` | 1280 × 400: sky, clouds, green hills, soap bubbles, a glass sheen and the Mihawk mark |
| `docs/media/readme/divider-{day,night}.svg` | the thin strip above six of the README's sections |
| `docs/media/readme/orb-<icon>-{day,night}.svg` | a glossy orb around one of the desktop's original icons (`themes/windows11/icons/original/`), one per section heading |
| `docs/media/readme/badges/<name>-{day,night}.svg` | the seven badges under the title: platform, license, status, tested on, runs, built on, languages |

**Rules the files keep.** Everything is drawn as shapes: no font file, script, `foreignObject` or external reference.
The banner, dividers and orbs hold no text. Only the clouds' and bubbles' drift and a few stars' twinkle move, through
SMIL inside the file. The badges draw their words in a generic sans-serif stack, each word pinned to its measured
width (`textLength`), so whatever face the reader's browser has fits the pill. Every file is under 8 KB.

**Licences.** The banner draws the Mihawk mark and each orb an original icon, so those files are LGPL-2.1-or-later like
the mark and the icons, and each carries its SPDX line. The badges are GPL-3.0-or-later like the rest of the
repository ([NOTICE.md](../NOTICE.md), "The README's art"; [LICENSING.md](../LICENSING.md)).

**Changing a badge.** A badge states a fact the tree supports. Update it when that fact changes: the tested consoles,
when a gate passes on another one; the GE-Proton pin, with `tools/fetch-all.sh`; the language count, with
`src/pw_setup.c`. Then run the script again and commit the files with the change. The two scripts below reproduce
every file byte for byte (checked 2026-10-27). They live here, not in `tools/`, until a gated commit moves them there.

```sh
python3 -I make_readme_art.py docs/media/readme .        # banner, dividers, orbs; reads themes/windows11/icons/original/
python3 -I make_badges.py docs/media/readme/badges        # badges; Pillow and Liberation Sans Bold (Arial's metrics)
```

<details>
<summary><code>make_readme_art.py</code></summary>

```python
# SPDX-License-Identifier: GPL-3.0-or-later
# Draw the README's Frutiger Aero art as self-contained SVGs: a banner, a section divider and an orb per heading icon,
# each in a day and a night variant. No text, fonts, scripts or foreignObject; SMIL only for a slow drift.
# usage, from the repository root: python3 -I make_readme_art.py docs/media/readme .
import re, sys
from pathlib import Path

out, repo = Path(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
LICENCE = '<!-- SPDX-License-Identifier: LGPL-2.1-or-later. MihawkOS README art, drawn for this project; the mark and icons are themes/windows11/\'s. -->'

DAY = dict(sky=('#3f97e6', '#8acdfb', '#e2f6ff'), far='#b8e9a0', mid=('#a3eb45', '#4fae2c'), near=('#3f9f2a', '#2a7d22'),
           crest='#ffffff', crest_op='.5', bub_op='1', rim=('#ffffff', '#ffc7ef', '#9ee6ff'), cloud='#ffffff', cloud_op='.9',
           sheen='.45', orb=('#ffffff', '#e9f8ff', '#9fd8ff', '#5fb6f2'), orb_rim='#ffffff', gloss='.9',
           bar=('#8acdfb', '#3f97e6', '#6cc534', '#a3eb45'))
NIGHT = dict(sky=('#030b1d', '#0b2348', '#174070'), far='#123b2c', mid=('#1f5a2a', '#123d1a'), near=('#0e3416', '#08240f'),
             crest='#8cf0a0', crest_op='.18', bub_op='.55', rim=('#cfe6ff', '#c9a6ff', '#6fd3ff'), cloud='#aac8f0', cloud_op='.07',
             sheen='.12', orb=('#4a7fb8', '#2a5a92', '#173f70', '#0b2548'), orb_rim='#7fb4ec', gloss='.35',
             bar=('#2b74c9', '#1b4f95', '#2f7d2a', '#4fae2c'))


def mark(x, y, size):
    """The Mihawk OS mark (themes/windows11/start.svg), drawn as its shapes."""
    s = size / 100
    return (f'<g transform="translate({x} {y}) scale({s:g})" filter="url(#lift)">'
            '<rect x="1" y="1" width="98" height="98" rx="30" fill="#fff" stroke="#c9d3dd" stroke-width="2"/>'
            '<rect x="6" y="6" width="88" height="88" rx="25" fill="none" stroke="#ffb020" stroke-width="2"/>'
            '<polyline points="25,80.5 25,34.5 50,64.5 75,34.5 75,80.5" fill="none" stroke="#0e2b46" stroke-width="11" '
            'stroke-miterlimit="8"/></g>')


def bubble(cx, cy, r, dur, rise):
    return (f'<g><animateTransform attributeName="transform" type="translate" values="0 0;0 -{rise};0 0" dur="{dur}s" '
            f'repeatCount="indefinite"/><circle cx="{cx}" cy="{cy}" r="{r}" fill="url(#bub)" stroke="url(#rim)" '
            f'stroke-width="{max(1, r / 14):.1f}"/><ellipse cx="{cx - r * .35:.1f}" cy="{cy - r * .45:.1f}" rx="{r * .32:.1f}" '
            f'ry="{r * .17:.1f}" fill="#fff" opacity=".85" transform="rotate(-30 {cx - r * .35:.1f} {cy - r * .45:.1f})"/></g>')


def cloud(x, y, w):
    h = w * .28
    return (f'<ellipse cx="{x}" cy="{y}" rx="{w / 2:g}" ry="{h / 2:g}"/><ellipse cx="{x - w * .18:g}" cy="{y - h * .35:g}" '
            f'rx="{w * .22:g}" ry="{h * .55:g}"/><ellipse cx="{x + w * .12:g}" cy="{y - h * .5:g}" rx="{w * .26:g}" ry="{h * .7:g}"/>')


def banner(t, night):
    W, H = 1280, 400
    sky = t['sky']
    stars = moon = sun = ''
    if night:
        pts = [(83, 41), (190, 112), (262, 30), (371, 78), (455, 24), (520, 130), (611, 58), (702, 20), (788, 96),
               (861, 44), (948, 140), (1003, 22), (1150, 150), (1222, 60), (1255, 128), (36, 150), (300, 170), (742, 160)]
        stars = ''.join(f'<circle cx="{x}" cy="{y}" r="{1 + (i % 3) * .5:g}" fill="#fff">' +
                        (f'<animate attributeName="opacity" values="1;.3;1" dur="{3 + i % 4}s" repeatCount="indefinite"/>' if i % 3 == 0 else '')
                        + '</circle>' for i, (x, y) in enumerate(pts))
        moon = ('<circle cx="1085" cy="86" r="70" fill="url(#glow)"/>'
                '<circle cx="1085" cy="86" r="30" fill="url(#moon)"/>')
    else:
        sun = ('<circle cx="1090" cy="10" r="300" fill="url(#glow)"/>'
               '<g fill="#fff" opacity=".18"><path d="M1090 10 L760 400 L840 400Z"/><path d="M1090 10 L940 400 L990 400Z"/>'
               '<path d="M1090 10 L1120 400 L1170 400Z"/></g>'
               '<g fill="none" stroke="#fff" stroke-linecap="round" opacity=".45"><path d="M0 40 C300 10 700 70 1280 22" stroke-width="2"/>'
               '<path d="M0 64 C380 34 760 88 1280 52" stroke-width="1.2"/></g>')
    clouds = (f'<g fill="{t["cloud"]}" opacity="{t["cloud_op"]}" filter="url(#soft)">'
              '<animateTransform attributeName="transform" type="translate" values="0 0;-40 0;0 0" dur="60s" repeatCount="indefinite"/>'
              + cloud(170, 120, 230) + cloud(520, 70, 260) + cloud(860, 150, 300) + cloud(1220, 110, 200) + '</g>')
    hills = (f'<path d="M0 270 C220 220 430 250 640 236 C860 222 1060 196 1280 226 V400 H0Z" fill="{t["far"]}"/>'
             '<path d="M0 316 C250 262 470 304 760 286 C1000 270 1160 272 1280 290 V400 H0Z" fill="url(#mid)"/>'
             f'<path d="M0 316 C250 262 470 304 760 286 C1000 270 1160 272 1280 290 V302 C1160 286 1000 286 760 302 C470 320 250 280 0 330Z" '
             f'fill="{t["crest"]}" opacity="{t["crest_op"]}"/>'
             '<path d="M0 364 C300 336 640 358 900 346 C1080 338 1200 342 1280 350 V400 H0Z" fill="url(#near)"/>')
    bubbles = (f'<g opacity="{t["bub_op"]}">' + bubble(150, 250, 34, 14, 10) + bubble(330, 120, 18, 11, 8) +
               bubble(440, 300, 46, 18, 12) + bubble(800, 90, 26, 13, 9) + bubble(905, 230, 52, 20, 14) +
               bubble(1010, 330, 14, 9, 6) + bubble(1180, 220, 30, 15, 10) + bubble(60, 80, 12, 10, 6) + '</g>')
    defs = (
        f'<linearGradient id="sky" x2="0" y2="1"><stop stop-color="{sky[0]}"/><stop offset=".6" stop-color="{sky[1]}"/>'
        f'<stop offset="1" stop-color="{sky[2]}"/></linearGradient>'
        f'<radialGradient id="glow"><stop stop-color="#fff" stop-opacity="{".9" if not night else ".35"}"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>'
        '<radialGradient id="moon" cx=".38" cy=".36"><stop stop-color="#fffdf2"/><stop offset=".6" stop-color="#e8eefc"/><stop offset="1" stop-color="#b9c9e8"/></radialGradient>'
        f'<linearGradient id="mid" x2="0" y2="1"><stop stop-color="{t["mid"][0]}"/><stop offset="1" stop-color="{t["mid"][1]}"/></linearGradient>'
        f'<linearGradient id="near" x2="0" y2="1"><stop stop-color="{t["near"][0]}"/><stop offset="1" stop-color="{t["near"][1]}"/></linearGradient>'
        '<radialGradient id="bub" cx=".5" cy=".5" r=".5"><stop offset=".55" stop-color="#fff" stop-opacity=".04"/>'
        '<stop offset=".85" stop-color="#bfeaff" stop-opacity=".22"/><stop offset="1" stop-color="#fff" stop-opacity=".5"/></radialGradient>'
        f'<linearGradient id="rim" x1="0" y1="0" x2="1" y2="1"><stop stop-color="{t["rim"][0]}"/><stop offset=".5" stop-color="{t["rim"][1]}"/>'
        f'<stop offset="1" stop-color="{t["rim"][2]}"/></linearGradient>'
        f'<linearGradient id="sheen" x2="0" y2="1"><stop stop-color="#fff" stop-opacity="{t["sheen"]}"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>'
        '<filter id="soft" x="-20%" y="-50%" width="140%" height="200%"><feGaussianBlur stdDeviation="6"/></filter>'
        '<filter id="lift" x="-30%" y="-30%" width="160%" height="170%"><feDropShadow dx="0" dy="6" stdDeviation="7" flood-color="#0a2850" flood-opacity=".35"/></filter>'
        f'<clipPath id="frame"><rect width="{W}" height="{H}" rx="28"/></clipPath>')
    body = (f'<rect width="{W}" height="{H}" fill="url(#sky)"/>' + stars + moon + sun + clouds + hills + bubbles +
            mark(W / 2 - 80, 104, 160) +
            f'<path d="M0 0 H{W} V120 C900 160 380 90 0 140Z" fill="url(#sheen)"/>'
            f'<rect x="1" y="1" width="{W - 2}" height="{H - 2}" rx="27" fill="none" stroke="#fff" stroke-opacity="{".7" if not night else ".2"}" stroke-width="2"/>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" role="img" '
            f'aria-label="MihawkOS">{LICENCE}<defs>{defs}</defs><g clip-path="url(#frame)">{body}</g></svg>')


def divider(t):
    b = t['bar']
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1280 24" width="1280" height="24">{LICENCE}<defs>'
            f'<linearGradient id="bar"><stop stop-color="{b[0]}" stop-opacity="0"/><stop offset=".2" stop-color="{b[0]}"/>'
            f'<stop offset=".5" stop-color="{b[1]}"/><stop offset=".72" stop-color="{b[2]}"/><stop offset=".9" stop-color="{b[3]}"/>'
            f'<stop offset="1" stop-color="{b[3]}" stop-opacity="0"/></linearGradient>'
            f'<linearGradient id="gloss"><stop stop-color="#fff" stop-opacity="0"/><stop offset=".5" stop-color="#fff" stop-opacity="{t["gloss"]}"/>'
            '<stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient></defs>'
            '<rect x="40" y="8" width="1200" height="8" rx="4" fill="url(#bar)"/>'
            '<rect x="120" y="9" width="1040" height="2.5" rx="1.25" fill="url(#gloss)"/>'
            f'<g fill="none" stroke="{b[0]}" stroke-width="1.2" opacity=".8"><circle cx="24" cy="12" r="5"/><circle cx="1256" cy="12" r="5"/>'
            '<circle cx="8" cy="12" r="2.5"/><circle cx="1272" cy="12" r="2.5"/></g></svg>')


def orb(t, icon_svg):
    o = t['orb']
    inner = re.sub(r'^<svg[^>]*>|</svg>\s*$', '', icon_svg.strip())
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">{LICENCE}<defs>'
            f'<radialGradient id="o-fill" cx=".5" cy=".3" r=".75"><stop stop-color="{o[0]}"/><stop offset=".38" stop-color="{o[1]}"/>'
            f'<stop offset=".78" stop-color="{o[2]}"/><stop offset="1" stop-color="{o[3]}"/></radialGradient>'
            f'<linearGradient id="o-gloss" x2="0" y2="1"><stop stop-color="#fff" stop-opacity="{t["gloss"]}"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>'
            '<filter id="o-drop" x="-20%" y="-20%" width="140%" height="150%"><feDropShadow dx="0" dy="2" stdDeviation="1.6" flood-color="#0a3a78" flood-opacity=".35"/></filter></defs>'
            f'<circle cx="32" cy="32" r="29" fill="url(#o-fill)" stroke="{t["orb_rim"]}" stroke-opacity=".9" stroke-width="1.5" filter="url(#o-drop)"/>'
            f'<svg x="15" y="15" width="34" height="34" viewBox="0 0 64 64">{inner}</svg>'
            '<ellipse cx="32" cy="17" rx="19" ry="11" fill="url(#o-gloss)"/></svg>')


ICONS = ('desktop', 'this-pc', 'downloads', 'app-generic', 'app-browser', 'games', 'folder', 'drive', 'document', 'documents')
for name, t, night in (('day', DAY, False), ('night', NIGHT, True)):
    (out / f'banner-{name}.svg').write_text(banner(t, night))
    (out / f'divider-{name}.svg').write_text(divider(t))
    for icon in ICONS:
        src = (repo / f'themes/windows11/icons/original/{icon}.svg').read_text()
        (out / f'orb-{icon}-{name}.svg').write_text(orb(t, src))
for f in sorted(out.iterdir()):
    print(f'{f.stat().st_size:6} {f.name}')
```

</details>

<details>
<summary><code>make_badges.py</code></summary>

```python
# SPDX-License-Identifier: GPL-3.0-or-later
# Glossy Aero badge pills for the README, day and night: label on Gloss blue, value on glass. Text is drawn with a
# generic sans-serif stack and pinned to its measured width (textLength), so any face fits the pill.
# usage, from the repository root: python3 -I make_badges.py docs/media/readme/badges (needs Pillow and Liberation Sans Bold)
import sys
from html import escape
from pathlib import Path
from PIL import ImageFont

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
FONT = ImageFont.truetype('/usr/share/fonts/liberation/LiberationSans-Bold.ttf', 11)  # Arial's metrics
H, PAD = 22, 8
BADGES = [  # file, label, value
    ('platform', 'platform', 'PlayStation 5'),
    ('license', 'license', 'GPL-3.0-or-later'),
    ('status', 'status', 'alpha · experimental'),
    ('tested-on', 'tested on', 'PS5 Pro 10.01 · PS5 13.40 / 13.60'),
    ('runs', 'runs', 'Windows x64 + x86 (WoW64)'),
    ('built-on', 'built on', 'Wine · GE-Proton11-7'),
    ('languages', 'languages', '10'),
]
THEMES = {
    'day': dict(label=('#4aa6f4', '#1b6fd2'), value=('#ffffff', '#dff1fd'), label_ink='#ffffff', value_ink='#0e2b46',
                edge='#1662bf', gloss='.55', shade='#0a3a78'),
    'night': dict(label=('#2f7bd0', '#123f7c'), value=('#1a3a66', '#0c2246'), label_ink='#ffffff', value_ink='#e8f2ff',
                  edge='#5f9be0', gloss='.22', shade='#000000'),
}


def width(text):
    return round(FONT.getlength(text))


def badge(label, value, t):
    lw, vw = width(label), width(value)
    a, b = lw + 2 * PAD, vw + 2 * PAD
    w = a + b
    r = H / 2
    text = ('font-family="Helvetica, Arial, \'Liberation Sans\', sans-serif" font-size="11" font-weight="700" '
            'lengthAdjust="spacingAndGlyphs"')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{H}" viewBox="0 0 {w} {H}" role="img" '
            f'aria-label="{escape(label)}: {escape(value)}"><title>{escape(label)}: {escape(value)}</title><defs>'
            f'<linearGradient id="l" x2="0" y2="1"><stop stop-color="{t["label"][0]}"/><stop offset="1" stop-color="{t["label"][1]}"/></linearGradient>'
            f'<linearGradient id="v" x2="0" y2="1"><stop stop-color="{t["value"][0]}"/><stop offset="1" stop-color="{t["value"][1]}"/></linearGradient>'
            f'<linearGradient id="g" x2="0" y2="1"><stop stop-color="#fff" stop-opacity="{t["gloss"]}"/><stop offset="1" stop-color="#fff" stop-opacity=".04"/></linearGradient>'
            f'<clipPath id="c"><rect width="{w}" height="{H}" rx="{r:g}"/></clipPath></defs>'
            f'<g clip-path="url(#c)"><rect width="{a}" height="{H}" fill="url(#l)"/><rect x="{a}" width="{b}" height="{H}" fill="url(#v)"/>'
            f'<rect x="1" y="1" width="{w - 2}" height="{H / 2 - 1:g}" rx="{r - 1:g}" fill="url(#g)"/></g>'
            f'<rect x=".5" y=".5" width="{w - 1}" height="{H - 1}" rx="{r - .5:g}" fill="none" stroke="{t["edge"]}" stroke-opacity=".55"/>'
            f'<text x="{PAD}" y="15" fill="{t["shade"]}" fill-opacity=".25" textLength="{lw}" {text}>{escape(label)}</text>'
            f'<text x="{PAD}" y="14.5" fill="{t["label_ink"]}" textLength="{lw}" {text}>{escape(label)}</text>'
            f'<text x="{a + PAD}" y="14.5" fill="{t["value_ink"]}" textLength="{vw}" {text}>{escape(value)}</text></svg>')


for name, label, value in BADGES:
    for theme, t in THEMES.items():
        (out / f'{name}-{theme}.svg').write_text(badge(label, value, t))
print(sum(f.stat().st_size for f in out.iterdir()), 'bytes in', len(list(out.iterdir())), 'files')
```

</details>
