/* SPDX-License-Identifier: GPL-3.0-or-later
 * The loading screen's own Frutiger Aero look (native/proton_loading.cpp draws it on the display): a sky over a
 * meadow, out of focus, with large soft bubbles; a glass globe whose meridians turn, with eight bubbles orbiting it on
 * a tilted ring; a clear glass tube whose green liquid flows along it (the load cannot be measured, so the bar is
 * indeterminate); and a frosted card for a tip. Everything is drawn here, from code: no picture is read.
 *
 * Canvas: PW_LOADING_WIDTH x PW_LOADING_HEIGHT pixels, row-major, 0xAARRGGBB (alpha unused, always 0xff). The text
 * (the game's name, the line under it, the tip, the footer) is drawn by the caller at the positions below, with its
 * own fonts. pw_loading_art_frame redraws only PW_LOADING_DYNAMIC: the caller copies that rectangle to the display. */
#ifndef PW_LOADING_ART_H
#define PW_LOADING_ART_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PW_LOADING_WIDTH = 1920,
    PW_LOADING_HEIGHT = 1080,
    /* the rectangle each frame changes: the globe, its orbit, the line under the name and the bar */
    PW_LOADING_DYN_X0 = 560, PW_LOADING_DYN_Y0 = 210, PW_LOADING_DYN_X1 = 1360, PW_LOADING_DYN_Y1 = 750,
    /* text, baselines in pixels */
    PW_LOADING_TITLE_Y = 600,     /* the game's name, centred, 64 px */
    PW_LOADING_LINE_Y = 652,      /* "Starting in Mihawk OS · 12 s", centred, 26 px, redrawn each frame */
    PW_LOADING_TIP_X = 124,       /* the tip card's text, left-aligned */
    PW_LOADING_TIP_LABEL_Y = 910, /* "Tip", 18 px */
    PW_LOADING_TIP_Y = 946,       /* first line of the tip, 22 px; later lines 30 px apart */
    PW_LOADING_TIP_WIDTH = 588,   /* the tip wraps to this */
    PW_LOADING_FOOTER_X = 1856,   /* "MihawkOS", right-aligned at this x */
    PW_LOADING_FOOTER_Y = 1024,
};

/* Text colours, 0xAARRGGBB. */
#define PW_LOADING_TEXT 0xffffffffu
#define PW_LOADING_TEXT_SOFT 0xffe6f6ffu
#define PW_LOADING_TEXT_LABEL 0xffc8fbffu
#define PW_LOADING_TEXT_SHADOW 0xff06287au /* drawn 2 px below the text at PW_LOADING_SHADOW_ALPHA */
#define PW_LOADING_SHADOW_ALPHA 75u

/* The backdrop alone (sky, meadow, soft bubbles), for every screen of the loading screen's family. */
void pw_loading_art_backdrop(uint32_t *canvas);

/* The still picture of a game's start: the backdrop, the orbit ring and the tip card. canvas holds the whole screen. */
void pw_loading_art_base(uint32_t *canvas);

/* The glass tube in any rectangle, its ends fully rounded: fraction 0..1 fills it to that point; a negative fraction
 * makes the liquid flow along it (indeterminate) by tick. */
void pw_loading_art_tube(uint32_t *canvas, float x0, float y0, float x1, float y1, float fraction, unsigned tick);

/* The globe with its orbit and bubbles at any size: radius is the globe's, the orbit is about 2x wide (a 16 px globe
 * spins in a 66 x 20 px box), for the small spinners beside a line of text. */
void pw_loading_art_spinner(uint32_t *canvas, float cx, float cy, float radius, unsigned tick);

/* One frame of the animation (about 14 a second): copies PW_LOADING_DYNAMIC from base into canvas, then draws the
 * orbiting bubbles, the turning globe and the flowing bar for tick. Both hold the whole screen. */
void pw_loading_art_frame(uint32_t *canvas, const uint32_t *base, unsigned tick);

/* 0xAARRGGBB colour blended over under by alpha 0..255. */
uint32_t pw_loading_blend(uint32_t under, uint32_t over, unsigned alpha);

#ifdef __cplusplus
}
#endif
#endif
