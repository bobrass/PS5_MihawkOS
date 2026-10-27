#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Three Frutiger Aero wallpapers, original artwork drawn only by this script (no photograph, no third-party image):

  bubble-earth  a glossy Earth resting on the grass among bubbles, a skyline behind: the Mihawk OS desktop's wallpaper
                (tools/build-wine-theme.py draws it into proton.bmp; the owner's choice, 2026-10-25, replacing Aero
                Meadow)
  glass-city    a meadow under cumulus, a tree line and a glass skyline, the sun's flare and soap bubbles
  aqua-world    above and below the waterline: a glass globe at the surface, an island skyline, caustics and bubbles

Seeded, so every run gives the same pixels. The soap bubble is Aero Meadow's (tools/make-mihawk-wallpaper.py, beside
this file). A full 1920x1080 scene takes about 15 s.

  make-aero-wallpapers.py OUTDIR [--size 1920x1080] [--only NAME]
"""
import argparse
import importlib.util
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

MEADOW = Path(__file__).resolve().parent / 'make-mihawk-wallpaper.py'
_spec = importlib.util.spec_from_file_location('meadow', MEADOW)
meadow = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(meadow)
smoothstep, over, bubble = meadow.smoothstep, meadow.over, meadow.bubble


def mix(a, b, t):
    t = np.asarray(t, dtype=np.float64)
    return a + (b - a) * (t[..., None] if t.ndim else t)

SEED = 20261025


# ------------------------------------------------------------------------------------------------------------- textures

def noise(w, h, cells, rng):
    """Smooth value noise: a random grid cells high, bicubic-upsampled."""
    gw, gh = max(2, int(cells * w / h)), max(2, int(cells))
    grid = Image.fromarray((rng.random((gh, gw)) * 255).astype(np.uint8), 'L')
    return np.asarray(grid.resize((w, h), Image.Resampling.BICUBIC), dtype=np.float64) / 255


def streaks(w, h, rows, stretch, rng):
    """Noise stretched sideways (stretch > 1) or upright (stretch < 1)."""
    gh, gw = max(2, int(rows)), max(2, int(rows * w / h / stretch))
    grid = Image.fromarray((rng.random((gh, gw)) * 255).astype(np.uint8), 'L')
    return np.asarray(grid.resize((w, h), Image.Resampling.BICUBIC), dtype=np.float64) / 255


def fbm(w, h, rng, base=3, octaves=6):
    total, amp, norm = np.zeros((h, w)), 1.0, 0.0
    for o in range(octaves):
        total += amp * noise(w, h, base * 2 ** o, rng)
        norm += amp
        amp *= 0.5
    return total / norm


def ridged(w, h, cells, rng):
    n = noise(w, h, cells, rng) * 0.6 + noise(w, h, cells * 2.1, rng) * 0.4
    return np.clip(1 - np.abs(n - 0.5) * 2, 0, 1) ** 6


def blur(a, radius):
    img = Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8), 'L')
    return np.asarray(img.filter(ImageFilter.GaussianBlur(radius)), dtype=np.float64) / 255


def shift(a, dy, dx):
    """a moved by (dy, dx) pixels, the edges repeated (np.roll would wrap one side into the other)."""
    h, w = a.shape
    rows = np.clip(np.arange(h) + dy, 0, h - 1)
    cols = np.clip(np.arange(w) + dx, 0, w - 1)
    return a[rows][:, cols]


def grid(w, h):
    y = np.linspace(0, 1, h)[:, None] * np.ones((1, w))
    x = np.ones((h, 1)) * np.linspace(0, 1, w)[None, :]
    return x, y


def gradient(y, stops):
    out = np.ones(y.shape + (3,)) * np.array(stops[0][1])
    for (p0, _), (p1, c1) in zip(stops, stops[1:]):
        out = mix(out, np.array(c1), smoothstep(p0, p1, y))
    return out


# ----------------------------------------------------------------------------------------------------------------- sky

def sky(w, h, top=(0.02, 0.30, 0.85), mid=(0.12, 0.52, 0.97), low=(0.62, 0.86, 1.0), horizon=0.68):
    x, y = grid(w, h)
    return gradient(y, [(0.0, top), (0.42 * horizon / 0.68, mid), (horizon, low)]), x, y


def sun(image, x, y, w, h, cx, cy, size=0.055, flare=True):
    """A white sun with a cyan halo, star rays, and a lens flare's ghosts toward the frame's centre."""
    aspect = w / h
    d = np.sqrt(((x - cx) * aspect) ** 2 + (y - cy) ** 2)
    over(image, (0.80, 0.95, 1.0), 0.65 * np.exp(-(d / 0.40) ** 2))
    over(image, (0.55, 0.95, 1.0), 0.55 * np.exp(-((d - size * 1.9) / 0.02) ** 2))  # the cyan ring
    over(image, (1.0, 1.0, 1.0), np.clip(1.3 * np.exp(-(d / size) ** 4), 0, 1))
    angle = np.arctan2(y - cy, (x - cx) * aspect)
    rays = sum(np.exp(-(((angle - a + math.pi) % (2 * math.pi) - math.pi) / 0.012) ** 2)
               for a in np.linspace(-math.pi, math.pi, 8, endpoint=False) + 0.3)
    over(image, (1.0, 1.0, 1.0), 0.7 * rays * np.exp(-(d / 0.18) ** 2))
    if flare:
        for t, r, colour, a in ((0.35, 0.020, (0.6, 1.0, 0.8), 0.18),
                                (0.75, 0.015, (1.0, 0.9, 0.6), 0.22), (1.15, 0.07, (0.6, 0.9, 1.0), 0.08)):
            fx, fy = cx + (0.5 - cx) * t * 2, cy + (0.5 - cy) * t * 2
            fd = np.sqrt(((x - fx) * aspect) ** 2 + (y - fy) ** 2)
            over(image, colour, a * smoothstep(r, r * 0.6, fd))


def clouds(image, x, y, w, h, rng, specs, light=(0.85, 0.12), lit=(1.0, 1.0, 1.0), shade=(0.70, 0.79, 0.92),
           alpha=1.0):
    """Cumulus: each spec (cx, cy, width, height) a heap of soft lumps with a flat base, eroded by fractal noise into
    billows. Lit by marching toward the sun through the density, so a cloud shades itself: sunlit faces white, deep
    parts and bases blue-grey, thin edges translucent and silver-lined."""
    aspect = w / h
    mask = np.zeros((h, w))
    for cx, cy, cw, ch in specs:
        local = np.zeros((h, w))
        for _ in range(int(24 + 90 * cw)):
            px = cx + rng.uniform(-0.5, 0.5) * cw
            top = 1 - (2 * (px - cx) / cw) ** 2
            py = cy + ch * 0.5 - rng.uniform(0.15, 0.85) * ch * max(top, 0.15)
            r = rng.uniform(0.30, 0.62) * ch * (0.55 + 0.45 * max(top, 0))
            q = ((x - px) * aspect / r) ** 2 + ((y - py) / r) ** 2
            local = np.maximum(local, np.exp(-q * 1.4))
        mask = np.maximum(mask, local * smoothstep(cy + ch * 0.58, cy + ch * 0.36, y))
    n = fbm(w, h, rng, base=6, octaves=7)
    fine = fbm(w, h, rng, base=40, octaves=3)
    density = smoothstep(0.28, 0.70, mask * (0.58 + 0.72 * n) + 0.08 * (fine - 0.5))
    soft = blur(density, 0.004 * h)
    dx, dy = np.sign(light[0] - 0.5) * 0.7, -1.0
    step = 0.006 * h
    accumulated = np.zeros((h, w))
    for i in range(1, 9):
        accumulated += shift(soft, int(i * step * dy), int(i * step * dx))
    sunlight = np.exp(-0.22 * accumulated)
    ambient = 0.35 + 0.65 * smoothstep(0.9, 0.2, blur(density, 0.03 * h))
    tone = np.clip(0.60 * sunlight + 0.40 * ambient + 0.12, 0, 1)
    colour = mix(np.ones(image.shape) * np.array(shade), np.array(lit), tone)
    colour = mix(colour, np.array([0.80, 0.90, 1.0]), 0.25 * (1 - density))  # thin parts take the sky's blue
    over(image, colour, np.clip(density * alpha, 0, 1))
    rim = np.clip(soft - density, 0, 1) * sunlight
    over(image, (1.0, 1.0, 1.0), np.clip(alpha * 1.6 * rim, 0, 1))  # the silver lining

def meadow_ground(image, x, y, w, h, rng, line, lit=(0.62, 0.92, 0.18), deep=(0.10, 0.48, 0.06), sun_x=0.85,
                  flowers=0.0):
    """Rolling grass from a ridge line down. Colour drifts between yellow-green and blue-green, finer toward the
    horizon (perspective); clumps and light patches; in the foreground, individual blades drawn as thin curved strokes
    with dark roots and lit tips, and a few small flowers."""
    depth = y - line
    cover = np.clip(depth * h / 1.5 + 0.5, 0, 1)
    grass = mix(np.ones(image.shape) * np.array(lit), np.array(deep), smoothstep(0.0, 0.50, depth))
    drift = fbm(w, h, rng, base=2, octaves=4)
    grass = mix(grass, np.array([0.30, 0.72, 0.22]), 0.25 * smoothstep(0.45, 0.7, drift))
    grass = mix(grass, np.array([0.85, 0.95, 0.35]), 0.18 * smoothstep(0.55, 0.3, drift) * smoothstep(0.3, 0.0, depth))
    for rows, stretch, amount, near in ((2, 0.0025, 0.16, 1.0), (4, 0.004, 0.12, 0.7), (10, 0.012, 0.10, 0.3)):
        blades = streaks(w, h, rows, stretch, rng)
        weight = smoothstep(0.0, 0.35 * near + 0.02, depth)
        grass = mix(grass, np.array(deep) * 0.7, amount * blades * weight)
        grass = mix(grass, np.array(lit) * 1.1, 0.5 * amount * (1 - blades) * weight)
    clumps = fbm(w, h, rng, base=8, octaves=4)
    grass = mix(grass, np.array(deep) * 0.80, 0.08 * smoothstep(0.55, 0.85, clumps) * smoothstep(0.03, 0.25, depth))
    grass = mix(grass, np.array([0.85, 1.0, 0.45]), 0.25 * np.exp(-((x - sun_x) / 0.35) ** 2) * np.exp(-(depth / 0.06) ** 2))
    over(image, np.clip(grass, 0, 1), cover)
    over(image, (0.90, 1.0, 0.70), 0.35 * cover * np.exp(-(depth * h / 3) ** 2))  # the sunlit crest
    # foreground blades, drawn on a 2x mask in three tones
    for tone, count, alpha in (((0.05, 0.30, 0.04), 3200, 0.55), ((0.30, 0.68, 0.12), 2600, 0.65),
                               ((0.72, 0.95, 0.35), 1300, 0.55)):
        big = Image.new('L', (w * 2, h * 2), 0)
        d = ImageDraw.Draw(big)
        for _ in range(count):
            bx = rng.uniform(0, w * 2)
            by = h * 2 * (1.0 - rng.uniform(0, 0.30) ** 1.3)
            length = h * 2 * rng.uniform(0.02, 0.07) * (by / (h * 2)) ** 3
            bend = rng.normal(0, 0.35) * length
            pts = [(bx + bend * (t / 4) ** 2, by - length * t / 4) for t in range(5)]
            d.line(pts, fill=255, width=max(1, int(rng.uniform(1.5, 3.5) * h / 1080)))
        m = np.asarray(big.resize((w, h), Image.Resampling.LANCZOS), dtype=np.float64) / 255
        over(image, tone, np.clip(alpha * m, 0, 1) * cover)
    if flowers:
        for _ in range(int(140 * flowers)):
            fx, fy = rng.uniform(0, 1), 1.0 - rng.uniform(0, 0.25) ** 1.4
            r = (1.5 + 4.5 * (fy - 0.75) / 0.25) * h / 1080
            c = (1.0, 1.0, 0.95) if rng.random() < 0.6 else (1.0, 0.88, 0.25)
            x0, x1, y0, y1 = int(fx * w - 3 * r), int(fx * w + 3 * r) + 1, int(fy * h - 3 * r), int(fy * h + 3 * r) + 1
            if x0 < 0 or y0 < 0 or x1 > w or y1 > h:
                continue
            yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float64)
            q = ((xx - fx * w) ** 2 + (yy - fy * h) ** 2) / (r * r)
            over(image[y0:y1, x0:x1], c, 0.95 * np.exp(-q * q))
            over(image[y0:y1, x0:x1], (1.0, 0.8, 0.2), 0.8 * np.exp(-q * 6))
    return cover

def tree_line(image, x, y, w, h, rng, base, height=0.035, colour=(0.18, 0.55, 0.15), lit=(0.45, 0.80, 0.25),
              x0=0.0, x1=1.0):
    """A band of round tree crowns along a horizon line, lit on top, hazier with distance."""
    aspect = w / h
    mask = np.zeros((h, w))
    lightmap = np.zeros((h, w))
    px = x0
    while px < x1:
        r = height * rng.uniform(0.5, 1.0)
        cy = base - r * rng.uniform(0.3, 0.9)
        q = ((x - px) * aspect) ** 2 + (y - cy) ** 2
        crown = smoothstep(r, r * 0.85, np.sqrt(q))
        mask = np.maximum(mask, crown)
        lightmap = np.maximum(lightmap, crown * smoothstep(cy + r * 0.4, cy - r * 0.8, y))
        px += r * rng.uniform(0.5, 1.1) / aspect
    mask *= (y < base + 0.01)
    leaves = noise(w, h, 260, rng)
    tone = mix(np.ones(image.shape) * np.array(colour), np.array(lit), np.clip(lightmap * 0.8 + 0.3 * (leaves - 0.5), 0, 1))
    over(image, tone, mask)




# ---------------------------------------------------------------------------------------------------------------- city

def skyline(image, x, y, w, h, rng, base, x0, x1, tallest=0.36, count=22, haze=(0.70, 0.88, 1.0), back=True,
            mask_out=None):
    reflect = image.copy()  # the sky and clouds behind, which the glass mirrors
    """Glass towers: each a sky-reflecting gradient with a lit and a shaded face, faint floor lines, some with a
    stepped crown or a spire. A paler back row sits behind the front one."""
    drawn = np.zeros((h, w))
    rows = ((0.55, 1.6, 0.55), (1.0, 1.0, 0.0)) if back else ((1.0, 1.0, 0.0),)
    for scale, density, hazed in rows:
        for _ in range(int(count * density)):
            cx = rng.uniform(x0, x1)
            bw = rng.uniform(0.010, 0.026) * (1.2 - 0.4 * scale)
            centre = 1 - abs((cx - (x0 + x1) / 2) / ((x1 - x0) / 2))
            bh = tallest * scale * rng.uniform(0.25, 1.0) * (0.45 + 0.55 * centre)
            left, right, top = cx - bw, cx + bw, base - bh
            c0, c1 = int(max(0, left * w)), int(min(w, right * w + 1))
            r0, r1 = int(max(0, (top - 0.05) * h)), int(min(h, base * h + 2))
            if c0 >= c1:
                continue
            xs, ys = x[r0:r1, c0:c1], y[r0:r1, c0:c1]
            u = (xs - left) / (right - left)
            crown = rng.integers(0, 4)
            roof = top + np.where(crown == 1, 0.012 * np.abs(u - 0.5) * 2, 0.0)  # pyramidal
            body = np.clip((xs - left) * w + 0.5, 0, 1) * np.clip((right - xs) * w + 0.5, 0, 1) \
                * np.clip((ys - roof) * h + 0.5, 0, 1) * (ys < base + 0.003)
            if crown == 2:  # a stepped top
                body = np.maximum(body, (np.abs(u - 0.5) < 0.3) * np.clip((ys - (top - 0.015)) * h, 0, 1) * (ys < top + 0.001))
            if crown == 3 and bh > 0.15:  # a spire
                body = np.maximum(body, (np.abs(xs - cx) * w < 1.2) * (ys > top - 0.06) * (ys < top + 0.001))
            t = np.clip((ys - top) / max(bh, 1e-3), 0, 1)
            glass = gradient(t, [(0.0, (0.75, 0.88, 1.0)), (0.5, (0.35, 0.58, 0.85)), (1.0, (0.20, 0.38, 0.65))])
            shaded = smoothstep(0.55, 0.62, u)
            glass = mix(glass, np.array([0.10, 0.22, 0.45]), 0.45 * shaded)
            # the glass mirrors the sky above and beside it, squeezed and shifted per tower
            rows_src = np.clip(((top - (ys - top) * 0.55 - 0.02) * h).astype(int), 0, h - 1)
            cols_src = np.clip((xs * w + rng.uniform(-0.08, 0.08) * w).astype(int), 0, w - 1)
            mirrored = reflect[rows_src, cols_src]
            glass = mix(glass, mirrored, 0.42 * (1 - 0.6 * shaded))
            panes = max(3, int(round(bw * w / 6)))
            column = (u * panes) % 1.0
            floor = ((ys - top) * h / max(3.0, 9 * scale)) % 1.0
            pane_id = np.floor(u * panes) * 7.3 + np.floor((ys - top) * h / max(3.0, 9 * scale)) * 3.1
            glass = mix(glass, np.array([0.55, 0.75, 1.0]), 0.10 * (0.5 + 0.5 * np.sin(pane_id * 12.9898)))
            mullion = np.maximum(smoothstep(0.08, 0.0, column), smoothstep(0.10, 0.0, floor))
            glass = mix(glass, np.array([0.12, 0.22, 0.38]), 0.35 * mullion)
            glass = mix(glass, np.array([1.0, 1.0, 1.0]), 0.35 * np.exp(-((u - 0.18) / 0.06) ** 2))  # the sheen
            glass = mix(glass, np.array([1.0, 1.0, 1.0]), 0.45 * np.exp(-((ys - top) * h / 1.5) ** 2))  # lit roof edge
            glass = mix(glass, np.array(haze), hazed)
            if mask_out is not None:
                body = body * (1 - mask_out[r0:r1, c0:c1])
            over(image[r0:r1, c0:c1], glass, body)
            drawn[r0:r1, c0:c1] = np.maximum(drawn[r0:r1, c0:c1], body)
    over(image, haze, 0.25 * drawn * smoothstep(base - 0.08, base, y))  # mist at their feet
    return drawn


# ------------------------------------------------------------------------------------------------------------- objects

def earth_texture(rng, size=512):
    return Image.fromarray(_earth_texture(rng, size).astype(np.float32), 'F').resize((size * 4, size * 2),
                                                                                    Image.Resampling.BICUBIC)


def _earth_texture(rng, size=512):
    """An equirectangular map: land where fractal noise is high, ocean elsewhere."""
    n = fbm(size * 2, size, rng, base=3, octaves=6)
    return n


def earth(image, x, y, w, h, cx, cy, r, texture, spin=0.6):
    """A glossy Earth: blue oceans, bright green land, a white rim glow and a big window highlight."""
    aspect = w / h
    dx, dy = (x - cx) * aspect / r, (y - cy) / r
    d2 = dx * dx + dy * dy
    inside = np.clip((1 - np.sqrt(d2)) * r * h / 1.5, 0, 1)
    z = np.sqrt(np.clip(1 - d2, 0, 1))
    lat = np.arcsin(np.clip(dy, -1, 1))
    lon = np.arctan2(dx, z) + spin
    if isinstance(texture, Image.Image):
        texture = np.asarray(texture, dtype=np.float64)
    th, tw = texture.shape
    u = ((lon / (2 * math.pi)) % 1.0) * (tw - 1)
    v = ((lat / math.pi) + 0.5) * (th - 1)
    land = texture[v.astype(int).clip(0, th - 1), u.astype(int).clip(0, tw - 1)]
    sea_level, peak = np.quantile(texture, 0.62), np.quantile(texture, 0.95)
    landmask = smoothstep(sea_level - 0.006, sea_level + 0.006, land)
    ocean = mix(np.ones(image.shape) * np.array([0.10, 0.55, 0.95]), np.array([0.02, 0.25, 0.70]), np.clip(1 - z, 0, 1))
    ground = mix(np.ones(image.shape) * np.array([0.55, 0.92, 0.25]), np.array([0.20, 0.65, 0.20]),
                 smoothstep(sea_level, peak, land))
    surface = mix(ocean, ground, landmask)
    shallow = smoothstep(sea_level - 0.04, sea_level, land) * (1 - landmask)
    surface = mix(surface, np.array([0.30, 0.85, 1.0]), 0.5 * shallow)  # turquoise shelves along the coasts
    cloudtex = np.roll(texture[::-1], tw // 3, axis=1)
    swirl = cloudtex[v.astype(int).clip(0, th - 1), ((u + 40 * np.sin(v / th * 9)) % tw).astype(int).clip(0, tw - 1)]
    cloud_level = np.quantile(texture, 0.80)
    surface = mix(surface, np.array([1.0, 1.0, 1.0]), 0.80 * smoothstep(cloud_level, cloud_level + 0.04, swirl))
    light = np.clip(0.55 + 0.5 * (-dx * 0.4 - dy * 0.6 + z * 0.7), 0.35, 1.25)
    surface = np.clip(surface * light[..., None], 0, 1)
    over(image, surface, inside)
    over(image, (0.75, 0.95, 1.0), inside * 0.6 * smoothstep(0.75, 1.0, np.sqrt(d2)))  # atmosphere at the rim
    hx, hy = (dx + 0.18) / 0.70, (dy + 0.45) / 0.42
    window = smoothstep(1.0, 0.6, np.sqrt(hx * hx + hy * hy))
    over(image, (1.0, 1.0, 1.0), inside * 0.55 * window * smoothstep(0.1, -0.6, dy))
    g = np.sqrt((dx + 0.42) ** 2 + (dy + 0.55) ** 2) / 0.06
    over(image, (1.0, 1.0, 1.0), inside * smoothstep(1.0, 0.2, g))


def glass_globe(image, x, y, w, h, cx, cy, r, tint=(0.25, 0.80, 0.95), deep=(0.02, 0.35, 0.72), meridians=True):
    """A glossy aqua orb with meridian lines, a darker rim, light gathered below and a window highlight."""
    aspect = w / h
    dx, dy = (x - cx) * aspect / r, (y - cy) / r
    d = np.sqrt(dx * dx + dy * dy)
    inside = np.clip((1 - d) * r * h / 1.5, 0, 1)
    body = mix(np.ones(image.shape) * np.array(tint), np.array(deep), smoothstep(0.2, 1.0, d) * 0.8 + 0.2 * smoothstep(-0.5, 1, dx))
    over(image, body, inside * 0.92)
    if meridians:
        z = np.sqrt(np.clip(1 - d * d, 0, 1))
        lon, lat = np.arctan2(dx, z), np.arcsin(np.clip(dy, -1, 1))
        lines = np.maximum(np.exp(-(np.sin(lon * 4) / 0.04) ** 2), np.exp(-(np.sin(lat * 4) / 0.04) ** 2))
        over(image, (0.80, 1.0, 1.0), inside * 0.18 * lines * smoothstep(1.0, 0.5, d))
    over(image, (0.70, 1.0, 1.0), inside * 0.45 * smoothstep(0.2, 0.95, dy) * smoothstep(1.0, 0.6, d))
    hx, hy = (dx + 0.18) / 0.50, (dy + 0.55) / 0.26
    over(image, (1.0, 1.0, 1.0), inside * 0.65 * smoothstep(1.0, 0.55, np.sqrt(hx * hx + hy * hy)) * smoothstep(-0.2, -0.9, dy + 0.1))
    over(image, (1.0, 1.0, 1.0), inside * 0.8 * np.exp(-((d - 0.985) / 0.012) ** 2))
    g = np.sqrt((dx + 0.40) ** 2 + (dy + 0.52) ** 2) / 0.06
    over(image, (1.0, 1.0, 1.0), inside * smoothstep(1.0, 0.2, g))
    return inside







def bubbles(image, w, h, rng, counts, area, sizes, strength=1.0, keep_clear=None, cluster=None):
    placed = []
    for size, n in zip(sizes, counts):
        for _ in range(n):
            for _attempt in range(80):
                r = size * h * rng.uniform(0.75, 1.2)
                if cluster:
                    ccx, ccy, spread = cluster
                    cx, cy = rng.normal(ccx, spread) * w, rng.normal(ccy, spread) * h
                else:
                    cx, cy = rng.uniform(area[0], area[1]) * w, rng.uniform(area[2], area[3]) * h
                if keep_clear and keep_clear(cx / w, cy / h):
                    continue
                if all(math.hypot(cx - px, cy - py) > (r + pr) * 1.05 for px, py, pr in placed):
                    placed.append((cx, cy, r))
                    break
    for cx, cy, r in sorted(placed, key=lambda b: b[2]):
        bubble(image, cx, cy, r, w, h, rng.uniform(0, 2 * math.pi), soft=1.2 if r < 0.07 * h else 0.04 * r,
               strength=strength * (0.7 if r < 0.02 * h else 1.0))


def sparkles(image, w, h, rng, n, area, colour=(1.0, 1.0, 1.0), size=(1.5, 6.0), tint=None):
    """Four-point star glints (the Aero sparkle), some tinted."""
    for _ in range(n):
        cx, cy = rng.uniform(area[0], area[1]) * w, rng.uniform(area[2], area[3]) * h
        r = rng.uniform(*size) * h / 1080
        span = int(16 * r)
        x0, x1, y0, y1 = int(cx - span), int(cx + span) + 1, int(cy - span), int(cy + span) + 1
        if x0 < 0 or y0 < 0 or x1 > w or y1 > h:
            continue
        yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float64)
        dx, dy = (xx - cx) / r, (yy - cy) / r
        glow = np.exp(-(dx * dx + dy * dy) * 0.8)
        glow = np.maximum(glow, np.exp(-(dx * dx) * 6 - (dy * dy) / 40))
        glow = np.maximum(glow, np.exp(-(dy * dy) * 6 - (dx * dx) / 40))
        c = tint[rng.integers(len(tint))] if tint and rng.random() < 0.6 else colour
        over(image[y0:y1, x0:x1], c, rng.uniform(0.5, 0.95) * np.clip(glow, 0, 1))


# -------------------------------------------------------------------------------------------------------------- scenes

def glass_city(w, h):
    rng = np.random.default_rng(SEED)
    image, x, y = sky(w, h, horizon=0.66)
    sun(image, x, y, w, h, 0.90, 0.12)
    clouds(image, x, y, w, h, rng, [(0.10, 0.58, 0.40, 0.16), (0.42, 0.60, 0.36, 0.12), (0.82, 0.50, 0.40, 0.22),
                                     (0.26, 0.26, 0.26, 0.10), (0.62, 0.20, 0.18, 0.07)], light=(0.90, 0.12))
    skyline(image, x, y, w, h, rng, 0.665, 0.60, 0.97, tallest=0.36, count=20)
    tree_line(image, x, y, w, h, rng, 0.675, height=0.030)
    meadow_ground(image, x, y, w, h, rng, 0.672 + 0.004 * np.sin(x * 9), sun_x=0.85, flowers=0.7)
    bubbles(image, w, h, rng, (1,), (0, 1, 0, 1), (0.095,), cluster=(0.53, 0.22, 0.001))
    bubbles(image, w, h, rng, (1,), (0, 1, 0, 1), (0.065,), cluster=(0.27, 0.53, 0.001))
    bubbles(image, w, h, rng, (3, 8), (0.20, 0.98, 0.05, 0.85), (0.03, 0.014),
            keep_clear=lambda bx, by: bx < 0.12 or math.hypot(bx - 0.90, by - 0.12) < 0.12)
    sparkles(image, w, h, rng, 12, (0.55, 1.0, 0.0, 0.40))
    return image


def bubble_earth(w, h):
    rng = np.random.default_rng(SEED + 1)
    image, x, y = sky(w, h, top=(0.04, 0.35, 0.86), mid=(0.15, 0.55, 0.98), low=(0.70, 0.90, 1.0), horizon=0.70)
    clouds(image, x, y, w, h, rng, [(0.28, 0.20, 0.38, 0.17), (0.66, 0.18, 0.30, 0.12), (0.72, 0.46, 0.26, 0.11),
                                     (0.08, 0.46, 0.24, 0.10), (0.95, 0.64, 0.24, 0.09)], light=(0.5, 0.0))
    skyline(image, x, y, w, h, rng, 0.80, 0.42, 0.97, tallest=0.485, count=26)
    meadow_ground(image, x, y, w, h, rng, 0.735 - 0.025 * np.sin(x * 4 + 0.6) - 0.01 * np.sin(x * 11), sun_x=0.6, flowers=0.4)
    noise(w, h, 300, rng)  # keeps the random sequence, so the rest of the scene stays as it was
    earth(image, x, y, w, h, 0.17, 0.74, 0.30, earth_texture(rng))
    bubbles(image, w, h, rng, (2, 5, 8), (0, 1, 0, 1), (0.06, 0.035, 0.018), cluster=(0.22, 0.80, 0.10))
    bubbles(image, w, h, rng, (2, 5, 6), (0, 1, 0, 1), (0.045, 0.03, 0.015), cluster=(0.93, 0.08, 0.06))
    sparkles(image, w, h, rng, 25, (0.0, 0.45, 0.55, 1.0), tint=[(1.0, 0.9, 0.5), (0.7, 1.0, 0.7)])
    return image


def aqua_world(w, h):
    rng = np.random.default_rng(SEED + 2)
    line = 0.60
    image, x, y = sky(w, h, top=(0.02, 0.38, 0.92), mid=(0.15, 0.60, 1.0), low=(0.80, 0.95, 1.0), horizon=line)
    clouds(image, x, y, w, h, rng, [(0.80, 0.42, 0.45, 0.26), (0.45, 0.50, 0.30, 0.12), (0.08, 0.48, 0.20, 0.06)],
           light=(0.2, 0.0))
    skyline(image, x, y, w, h, rng, line - 0.012, 0.68, 0.99, tallest=0.22, count=18)
    tree_line(image, x, y, w, h, rng, line, height=0.012, x0=0.66, x1=1.0)
    water_line = line + 0.006 * np.sin(x * 16 + 1.0) + 0.003 * np.sin(x * 41)
    below = np.clip((y - water_line) * h / 1.5 + 0.5, 0, 1)
    t = np.clip((y - line) / (1 - line), 0, 1)
    under = gradient(t, [(0.0, (0.35, 0.90, 1.0)), (0.35, (0.10, 0.62, 0.92)), (1.0, (0.02, 0.30, 0.62))])
    under = mix(under, np.array([0.85, 1.0, 1.0]), np.clip(0.6 * ridged(w, h, 16, rng) * smoothstep(0.0, 0.2, t) * (1 - t), 0, 1))
    rays = sum(np.exp(-(((x - c) - 0.3 * (y - line)) / wd) ** 2) for c, wd in
               ((0.30, 0.03), (0.40, 0.02), (0.52, 0.04), (0.60, 0.018), (0.72, 0.03)))
    under = mix(under, np.array([0.85, 1.0, 1.0]), 0.25 * rays * smoothstep(1.0, line, y))
    gx, gy, gr = 0.19, 0.50, 0.21
    inside = glass_globe(image, x, y, w, h, gx, gy, gr)  # drawn first: the water covers its lower part
    over(image, under, below * (1 - 0.50 * inside))
    over(image, (0.90, 1.0, 1.0), 0.8 * np.exp(-((y - water_line) * h / 2.5) ** 2))
    over(image, (0.70, 0.98, 1.0), 0.35 * np.exp(-((y - water_line) / 0.025) ** 2) * (y > water_line))
    # the splash where the globe meets the water: a foam ring, a lip of spray, droplets thrown up and out
    half = math.sqrt(max(gr * gr - (line - gy) ** 2, 1e-6))  # the globe's half-width at the waterline
    ring = np.sqrt(((x - gx) * w / h / half) ** 2 + ((y - line) / 0.016) ** 2)
    visible = np.where(y < line, 1 - inside, 1.0)  # the back of the ring is behind the globe
    foam = np.exp(-((ring - 1.0) / 0.10) ** 2) * (0.55 + 0.45 * noise(w, h, 140, rng)) * visible
    over(image, (0.96, 1.0, 1.0), 0.85 * foam * smoothstep(0.7, 1.0, ring))
    lip = np.exp(-((ring - 1.05) / 0.25) ** 2) * (y < line) * smoothstep(0.5, 0.8, noise(w, h, 70, rng)) * (1 - inside)
    over(image, (0.90, 1.0, 1.0), 0.45 * lip)
    for _ in range(70):
        side = rng.choice((-1, 1))
        a = rng.uniform(0.05, 1.2)
        dist = half * rng.uniform(1.05, 1.5)
        dxp = gx + side * math.cos(a) * dist * h / w
        dyp = line - math.sin(a) * dist * rng.uniform(0.15, 0.5)
        if math.hypot((dxp - gx) * w / h, dyp - gy) < gr * 1.03:
            continue  # never inside the globe
        bubble(image, dxp * w, dyp * h, rng.uniform(0.002, 0.007) * h, w, h, rng.uniform(0, 6.3), strength=0.9)
    bubbles(image, w, h, rng, (1, 4, 10, 24), (0.30, 0.98, 0.70, 0.98), (0.07, 0.04, 0.022, 0.010),
            keep_clear=lambda bx, by: math.hypot((bx - gx) * 1.78, by - gy) < gr + 0.03 or by < line + 0.09)
    bubbles(image, w, h, rng, (2, 4), (0.30, 0.65, 0.05, 0.45), (0.04, 0.018))
    sparkles(image, w, h, rng, 20, (0.05, 0.40, 0.25, 0.65))
    return image




SCENES = {'glass-city': glass_city, 'bubble-earth': bubble_earth, 'aqua-world': aqua_world}


def render(name, w, h):
    """Scene name at w x h as an RGB picture (what the build and tools/publication-check.py use)."""
    image = np.clip(SCENES[name](w, h), 0, 1)
    return Image.fromarray((image * 255 + 0.5).astype(np.uint8), 'RGB')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('out', type=Path)
    parser.add_argument('--size', default='1920x1080')
    parser.add_argument('--only')
    args = parser.parse_args()
    w, h = map(int, args.size.split('x'))
    args.out.mkdir(parents=True, exist_ok=True)
    for name, scene in SCENES.items():
        if args.only and name != args.only:
            continue
        render(name, w, h).save(args.out / f'{name}.png', optimize=True)
        print(name, flush=True)


if __name__ == '__main__':
    main()
