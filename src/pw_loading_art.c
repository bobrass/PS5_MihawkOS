/* SPDX-License-Identifier: GPL-3.0-or-later
 * The loading screen's Frutiger Aero look; the contract is in pw_loading_art.h. */
#include "pw_loading_art.h"

#include <math.h>
#include <string.h>

#define W PW_LOADING_WIDTH
#define H PW_LOADING_HEIGHT
#define PI 3.14159265f

/* the globe and the tilted ring its bubbles orbit on */
#define GLOBE_X 960.0f
#define GLOBE_Y 350.0f
#define GLOBE_R 100.0f
#define ORBIT_RX 205.0f
#define ORBIT_RY 60.0f
#define ORBS 8
/* the glass tube */
#define BAR_X0 580.0f
#define BAR_X1 1340.0f
#define BAR_Y0 700.0f
#define BAR_Y1 728.0f

typedef struct { float r, g, b; } Rgb;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float smooth(float e0, float e1, float x)
{
    const float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
static Rgb lerp(Rgb a, Rgb b, float t) { return (Rgb){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
static Rgb rgbf(float r, float g, float b) { return (Rgb){r, g, b}; }
static uint32_t pack(Rgb c)
{
    return 0xff000000u | ((uint32_t)clampf(c.r + 0.5f, 0, 255) << 16) | ((uint32_t)clampf(c.g + 0.5f, 0, 255) << 8) |
           (uint32_t)clampf(c.b + 0.5f, 0, 255);
}
static Rgb unpack(uint32_t p) { return rgbf((float)((p >> 16) & 0xff), (float)((p >> 8) & 0xff), (float)(p & 0xff)); }

uint32_t pw_loading_blend(uint32_t under, uint32_t over, unsigned alpha)
{
    return pack(lerp(unpack(under), unpack(over), (float)(alpha > 255 ? 255 : alpha) / 255.0f));
}

static void put(uint32_t *canvas, int x, int y, Rgb c, float a)
{
    if (x < 0 || y < 0 || x >= W || y >= H || a <= 0.0f) return;
    uint32_t *p = &canvas[(size_t)y * W + (size_t)x];
    *p = pack(lerp(unpack(*p), c, clampf(a, 0.0f, 1.0f)));
}

/* ------------------------------------------------------------------------------------------------------- backdrop */

/* large soft bubbles, out of focus: x, y, radius (pixels) */
static const float BOKEH[][3] = {{180, 170, 120}, {1650, 140, 90}, {1760, 560, 150}, {330, 610, 70},
                                 {1430, 820, 60},  {1560, 330, 36},  {620, 120, 46},  {1250, 120, 28}};

static Rgb backdrop(int x, int y)
{
    const float u = (float)x / (float)W, t = (float)y / (float)H;
    Rgb sky = lerp(rgbf(8, 74, 210), rgbf(40, 135, 240), smooth(0.0f, 0.5f, t));
    sky = lerp(sky, rgbf(165, 225, 255), smooth(0.45f, 0.76f, t));
    const float horizon = 0.775f + 0.02f * sinf(u * 3.3f + 0.6f);
    const float m = smooth(horizon - 0.02f, horizon + 0.02f, t); /* soft: the meadow is out of focus */
    const Rgb grass = lerp(rgbf(150, 230, 70), rgbf(40, 140, 30), smooth(horizon, 1.0f, t));
    Rgb c = lerp(sky, grass, m);
    /* a pale glow behind the globe and the name */
    const float gx = (u - 0.5f) * 1.78f, gy = t - 0.36f;
    c = lerp(c, rgbf(255, 255, 255), 0.22f * expf(-(gx * gx + gy * gy) / (0.42f * 0.42f)));
    /* the edges darken toward deep blue */
    const float vx = (u - 0.5f) * 1.5f, vy = (t - 0.45f) * 1.2f;
    c = lerp(c, rgbf(4, 30, 100), 0.45f * smooth(0.55f, 1.1f, sqrtf(vx * vx + vy * vy)));
    return c;
}

static void bokeh(uint32_t *canvas)
{
    for (unsigned i = 0; i < sizeof(BOKEH) / sizeof(BOKEH[0]); ++i) {
        const float cx = BOKEH[i][0], cy = BOKEH[i][1], r = BOKEH[i][2];
        for (int y = (int)(cy - r - 4); y <= (int)(cy + r + 4); ++y)
            for (int x = (int)(cx - r - 4); x <= (int)(cx + r + 4); ++x) {
                const float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) / r;
                if (d > 1.06f) continue;
                const float rim = expf(-((d - 0.94f) / 0.09f) * ((d - 0.94f) / 0.09f));
                const float body = smooth(1.02f, 0.90f, d) * (0.04f + 0.05f * smooth(0.2f, 0.9f, ((float)y - cy) / r));
                put(canvas, x, y, rgbf(235, 250, 255), 0.11f * rim + body); /* out of focus: a soft rim, light below */
            }
    }
}

/* A rounded rectangle's signed distance (negative inside). */
static float rounded(float x, float y, float x0, float y0, float x1, float y1, float r)
{
    const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
    const float qx = fabsf(x - cx) - hx, qy = fabsf(y - cy) - hy;
    const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
}

static void tip_card(uint32_t *canvas)
{
    const float x0 = 96, y0 = 872, x1 = 736, y1 = 1004, r = 18;
    for (int y = (int)y0 - 2; y <= (int)y1 + 2; ++y)
        for (int x = (int)x0 - 2; x <= (int)x1 + 2; ++x) {
            const float d = rounded((float)x + 0.5f, (float)y + 0.5f, x0, y0, x1, y1, r);
            const float inside = clampf(0.5f - d, 0, 1);
            const float t = ((float)y - y0) / (y1 - y0);
            put(canvas, x, y, rgbf(255, 255, 255), inside * (0.30f - 0.18f * t)); /* frosted, brighter on top */
            put(canvas, x, y, rgbf(255, 255, 255), 0.45f * clampf(1.0f - fabsf(d + 0.5f), 0, 1)); /* its rim */
            if (y < (int)y0 + 3) put(canvas, x, y, rgbf(255, 255, 255), 0.5f * inside); /* the lit top edge */
        }
}

static void orbit_ring(uint32_t *canvas, float cx, float cy, float k, int front_only)
{
    const float rx = ORBIT_RX * k, ry = ORBIT_RY * k;
    for (int y = (int)(cy - ry - 4); y <= (int)(cy + ry + 4); ++y)
        for (int x = (int)(cx - rx - 4); x <= (int)(cx + rx + 4); ++x) {
            const float dx = ((float)x - cx) / rx, dy = ((float)y - cy) / ry;
            const float d = sqrtf(dx * dx + dy * dy);
            if (front_only && (float)y < cy) continue; /* the ring's near half passes in front of the globe */
            const float px = fabsf(d - 1.0f) * ry; /* about the distance in pixels near the narrow ends */
            put(canvas, x, y, rgbf(200, 245, 255), 0.35f * clampf(1.5f * (k < 0.5f ? 0.8f : 1.0f) - px, 0, 1));
        }
}

void pw_loading_art_backdrop(uint32_t *canvas)
{
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) canvas[(size_t)y * W + (size_t)x] = pack(backdrop(x, y));
    bokeh(canvas);
}

void pw_loading_art_base(uint32_t *canvas)
{
    pw_loading_art_backdrop(canvas);
    orbit_ring(canvas, GLOBE_X, GLOBE_Y, 1.0f, 0);
    tip_card(canvas);
}

/* ---------------------------------------------------------------------------------------------------- animation */

/* A soap bubble of radius r: a faint body, a bright rim and a highlight; s scales it (a farther one is fainter). */
static void bubble(uint32_t *canvas, float cx, float cy, float r, float s)
{
    for (int y = (int)(cy - r - 2); y <= (int)(cy + r + 2); ++y)
        for (int x = (int)(cx - r - 2); x <= (int)(cx + r + 2); ++x) {
            const float dx = ((float)x + 0.5f - cx) / r, dy = ((float)y + 0.5f - cy) / r;
            const float d = sqrtf(dx * dx + dy * dy);
            const float cover = clampf((1.0f - d) * r + 0.5f, 0, 1);
            if (cover <= 0) continue;
            put(canvas, x, y, rgbf(120, 235, 255), cover * 0.25f * s);
            put(canvas, x, y, rgbf(235, 252, 255), cover * 0.85f * s * smooth(0.72f, 0.98f, d));
            const float hx = dx + 0.36f, hy = dy + 0.40f;
            put(canvas, x, y, rgbf(255, 255, 255), cover * 0.9f * s * smooth(0.30f, 0.12f, sqrtf(hx * hx + hy * hy)));
        }
}

/* The glass globe; its meridians turn with spin (radians). */
static void globe(uint32_t *canvas, float cx, float cy, float r, float spin)
{
    for (int y = (int)(cy - r - 2); y <= (int)(cy + r + 2); ++y)
        for (int x = (int)(cx - r - 2); x <= (int)(cx + r + 2); ++x) {
            const float dx = ((float)x + 0.5f - cx) / r, dy = ((float)y + 0.5f - cy) / r;
            const float d2 = dx * dx + dy * dy, d = sqrtf(d2);
            const float cover = clampf((1.0f - d) * r + 0.5f, 0, 1);
            if (cover <= 0) continue;
            const float lx = dx + 0.20f, ly = dy + 0.30f;
            const float shade = clampf(sqrtf(lx * lx + ly * ly) / 1.25f, 0, 1);
            Rgb c = lerp(rgbf(95, 231, 255), rgbf(26, 166, 236), smooth(0.0f, 0.45f, shade));
            c = lerp(c, rgbf(10, 95, 207), smooth(0.45f, 0.8f, shade));
            c = lerp(c, rgbf(6, 63, 158), smooth(0.8f, 1.0f, shade));
            c = lerp(c, rgbf(150, 255, 250), 0.45f * smooth(0.2f, 0.95f, dy) * smooth(1.0f, 0.5f, d)); /* light below */
            const float z = sqrtf(clampf(1.0f - d2, 0, 1));
            const float lon = atan2f(dx, z) + spin, lat = asinf(clampf(dy, -1, 1));
            const float meridian = expf(-(sinf(lon * 3.0f) / 0.05f) * (sinf(lon * 3.0f) / 0.05f)) * z;
            const float parallel = expf(-(sinf(lat * 3.0f) / 0.05f) * (sinf(lat * 3.0f) / 0.05f));
            c = lerp(c, rgbf(210, 250, 255), 0.35f * (meridian > parallel ? meridian : parallel) * smooth(1.0f, 0.6f, d));
            const float hx = dx / 0.66f, hy = (dy + 0.52f) / 0.36f;
            c = lerp(c, rgbf(255, 255, 255), 0.82f * smooth(1.0f, 0.65f, sqrtf(hx * hx + hy * hy)) * smooth(0.1f, -0.7f, dy));
            c = lerp(c, rgbf(255, 255, 255), 0.6f * expf(-((d - 0.985f) / 0.015f) * ((d - 0.985f) / 0.015f)));
            const float gx = dx + 0.42f, gy = dy + 0.55f;
            c = lerp(c, rgbf(255, 255, 255), smooth(0.07f, 0.02f, sqrtf(gx * gx + gy * gy)));
            put(canvas, x, y, c, cover);
        }
}

static void orbs(uint32_t *canvas, float cx, float cy, float k, unsigned tick, int front)
{
    for (int i = 0; i < ORBS; ++i) {
        const float a = (float)tick * 0.11f + (float)i * 2.0f * PI / ORBS;
        const float depth = (sinf(a) + 1.0f) / 2.0f; /* 1 nearest the viewer, at the ring's front */
        if ((depth >= 0.5f) != front) continue;
        const float size = (9.0f + 9.0f * depth * (0.75f + 0.25f * (float)(i % 3) / 2.0f)) * k;
        bubble(canvas, cx + cosf(a) * ORBIT_RX * k, cy + sinf(a) * ORBIT_RY * k, size > 2.5f ? size : 2.5f,
               0.4f + 0.6f * depth);
    }
}

void pw_loading_art_tube(uint32_t *canvas, float x0, float y0, float x1, float y1, float fraction, unsigned tick)
{
    const float r = (y1 - y0) / 2, length = x1 - x0;
    const float run = length * 0.37f; /* the flowing liquid's length */
    float tail, head;
    if (fraction < 0) {
        head = fmodf((float)tick * 16.0f, length + run) + x0; /* enters and leaves through the ends */
        tail = head - run;
    } else {
        tail = x0 - 40.0f;
        head = x0 + length * clampf(fraction, 0, 1);
    }
    const float soft = fraction < 0 ? 30.0f : 2.0f;
    for (int y = (int)y0 - 2; y <= (int)y1 + 2; ++y)
        for (int x = (int)x0 - 2; x <= (int)x1 + 2; ++x) {
            const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            const float d = rounded(fx, fy, x0, y0, x1, y1, r);
            const float inside = clampf(0.5f - d, 0, 1);
            const float t = (fy - y0) / (y1 - y0);
            put(canvas, x, y, rgbf(255, 255, 255), inside * (0.32f - 0.22f * smooth(0.0f, 0.55f, t) + 0.12f * smooth(0.7f, 1.0f, t)));
            put(canvas, x, y, rgbf(0, 30, 90), inside * 0.30f * smooth(0.25f, 0.0f, t)); /* the inner shadow on top */
            const float fill = clampf(0.5f - (d + 2.0f), 0, 1) * smooth(tail, tail + soft, fx) * smooth(head, head - soft, fx);
            if (fill > 0) {
                Rgb liquid = lerp(rgbf(182, 255, 122), rgbf(85, 213, 42), smooth(0.0f, 0.48f, t));
                liquid = lerp(liquid, rgbf(47, 174, 30), smooth(0.47f, 0.52f, t));
                liquid = lerp(liquid, rgbf(115, 232, 63), smooth(0.55f, 1.0f, t));
                put(canvas, x, y, liquid, fill);
                const float stripe = smooth(0.08f, 0.14f, t) * smooth(0.44f, 0.30f, t);
                put(canvas, x, y, rgbf(255, 255, 255), fill * 0.75f * stripe * (1.0f - 0.6f * t));
            }
            put(canvas, x, y, rgbf(255, 255, 255), 0.75f * clampf(1.0f - fabsf(d + 0.5f), 0, 1)); /* the glass rim */
        }
    if (fraction >= 0) return;
    /* fizz: small rings rising in the flowing liquid */
    for (int k = 0; k < 7; ++k) {
        const float phase = fmodf((float)tick * 0.07f + (float)k * 0.37f, 1.0f);
        const float bx = tail + fmodf((float)k * 61.7f + 23.0f, run);
        if (bx < x0 + r || bx > x1 - r) continue;
        const float by = y1 - 5.0f - phase * (y1 - y0 - 9.0f), br = 2.6f;
        for (int y = (int)(by - 4); y <= (int)(by + 4); ++y)
            for (int x = (int)(bx - 4); x <= (int)(bx + 4); ++x) {
                const float dd = sqrtf(((float)x + 0.5f - bx) * ((float)x + 0.5f - bx) + ((float)y + 0.5f - by) * ((float)y + 0.5f - by));
                put(canvas, x, y, rgbf(255, 255, 255), (1.0f - phase) * 0.8f * clampf(1.0f - fabsf(dd - br), 0, 1));
            }
    }
}

void pw_loading_art_spinner(uint32_t *canvas, float cx, float cy, float radius, unsigned tick)
{
    const float k = radius / GLOBE_R;
    orbit_ring(canvas, cx, cy, k, 0);
    orbs(canvas, cx, cy, k, tick, 0);
    globe(canvas, cx, cy, radius, (float)tick * 0.045f);
    orbit_ring(canvas, cx, cy, k, 1);
    orbs(canvas, cx, cy, k, tick, 1);
}

void pw_loading_art_frame(uint32_t *canvas, const uint32_t *base, unsigned tick)
{
    for (int y = PW_LOADING_DYN_Y0; y < PW_LOADING_DYN_Y1; ++y)
        memcpy(&canvas[(size_t)y * W + PW_LOADING_DYN_X0], &base[(size_t)y * W + PW_LOADING_DYN_X0],
               (size_t)(PW_LOADING_DYN_X1 - PW_LOADING_DYN_X0) * sizeof(uint32_t));
    orbs(canvas, GLOBE_X, GLOBE_Y, 1.0f, tick, 0);
    globe(canvas, GLOBE_X, GLOBE_Y, GLOBE_R, (float)tick * 0.045f);
    orbit_ring(canvas, GLOBE_X, GLOBE_Y, 1.0f, 1);
    orbs(canvas, GLOBE_X, GLOBE_Y, 1.0f, tick, 1);
    pw_loading_art_tube(canvas, BAR_X0, BAR_Y0, BAR_X1, BAR_Y1, -1.0f, tick);
}
