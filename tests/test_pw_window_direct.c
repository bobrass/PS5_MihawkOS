/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Direct flip (src/pw_window_direct.h) against the compositor itself: whenever it says yes, composing the same scene
 * must give exactly the game's frame; the cases it refuses show why (the composed screen differs). A 16x9 screen. */
#include "../src/pw_window_direct.h"
#include "../src/pw_window_effects.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { W = 16, H = 9, GAME = 0x20, DESK = 0x10, TOP = 0x30 };
static uint32_t desktop[W * H], game_gdi[W * H], frame[W * H], above[W * H], out[W * H];
static PwWindowSnapshot s;
static PwWindowImage images[4];
static PwWindowEffects effects;

/* The game full-screen and alone above the desktop; the case then changes what it tests. */
static void scene(void)
{
    memset(&s, 0, sizeof(s));
    s.version = PW_WINDOW_SNAPSHOT_VERSION; s.size = sizeof(s); s.count = 2; s.clip_count = 1;
    s.clips[0] = (PwWindowClip){0, 0, W, H};
    s.windows[0] = (PwWindowRow){.hwnd = GAME, .pid = 700, .generation = 5, .flags = PW_WINDOW_VISIBLE | PW_WINDOW_CLIENT,
        .right = W, .bottom = H, .client_right = W, .client_bottom = H, .clip_count = 1};
    s.windows[1] = (PwWindowRow){.hwnd = DESK, .pid = 600, .generation = 1, .flags = PW_WINDOW_VISIBLE | PW_WINDOW_DESKTOP,
        .right = W, .bottom = H};
    images[0] = (PwWindowImage){.hwnd = DESK, .pid = 600, .generation = 1, .width = W, .height = H, .stride = W * 4,
        .pixels = desktop, .bytes = sizeof(desktop), .sequence = 1};
    images[1] = (PwWindowImage){.hwnd = GAME, .pid = 700, .generation = 5, .width = W, .height = H, .stride = W * 4,
        .pixels = game_gdi, .bytes = sizeof(game_gdi), .sequence = 1};
    images[2] = (PwWindowImage){.hwnd = GAME, .pid = 700, .generation = 5, .plane = PW_WINDOW_GPU, .width = W,
        .height = H, .stride = W * 4, .pixels = frame, .bytes = sizeof(frame), .sequence = 1};
    images[3] = (PwWindowImage){.hwnd = TOP, .pid = 800, .generation = 9, .width = W, .height = H, .stride = W * 4,
        .pixels = above, .bytes = sizeof(above), .sequence = 1};
}
/* A window above the game, at the top of the stack. */
static void add_above(int32_t left, int32_t top, int32_t right, int32_t bottom, uint32_t flags, uint8_t shadow)
{
    memmove(&s.windows[1], &s.windows[0], sizeof(s.windows[0]) * s.count);
    s.windows[0] = (PwWindowRow){.hwnd = TOP, .pid = 800, .generation = 9, .flags = flags, .left = left, .top = top,
        .right = right, .bottom = bottom, .client_left = left, .client_top = top, .client_right = right,
        .client_bottom = bottom, .shadow = shadow};
    ++s.count;
}
static const PwWindowRow *game(void)
{
    for (uint32_t i = 0; i < s.count; ++i) if (s.windows[i].hwnd == GAME) return &s.windows[i];
    return NULL;
}
/* The verdict, checked against the composed screen: yes only where composing gives the frame. */
static int check(int expected, uint32_t frame_width, uint32_t frame_height)
{
    const PwWindowRow *row = game();
    const int verdict = pw_window_direct_flip(&s, 700, GAME, row ? row->generation : 5, frame_width, frame_height, W, H);
    if (verdict != expected)
        fprintf(stderr, "verdict %s, expected %s\n", pw_window_direct_reason(verdict), pw_window_direct_reason(expected));
    assert(verdict == expected);
    if (pw_windows_compose_effects(out, W, H, &s, images, 4, &effects) == 0) {
        const int same = !memcmp(out, frame, sizeof(frame));
        if (verdict == PW_DIRECT_YES) assert(same);
        return same;
    }
    assert(verdict != PW_DIRECT_YES);
    return -1;
}

int main(void)
{
    for (unsigned i = 0; i < W * H; ++i) {
        desktop[i] = 0xff102030u; game_gdi[i] = 0xff405060u; above[i] = 0xff708090u;
        frame[i] = 0xff000000u | (i * 2654435761u >> 8);  /* every pixel its own value */
    }
    scene(); check(PW_DIRECT_YES, W, H);
    /* Wine's clips may be several rectangles: the two halves of the screen are still all of it. */
    scene(); s.clip_count = 2; s.windows[0].clip_count = 2;
    s.clips[0] = (PwWindowClip){0, 0, W, 4}; s.clips[1] = (PwWindowClip){0, 4, W, H};
    check(PW_DIRECT_YES, W, H);
    /* A game in a child window filling the screen, its frame window below it. */
    scene(); s.windows[0].flags |= PW_WINDOW_CHILD; ++s.count; s.windows[2] = s.windows[1];
    s.windows[1] = (PwWindowRow){.hwnd = 0x21, .pid = 700, .generation = 6, .flags = PW_WINDOW_VISIBLE, .right = W, .bottom = H};
    check(PW_DIRECT_YES, W, H);
    /* Windows above that cannot reach the screen: hidden, or beyond its edge. */
    scene(); add_above(0, 6, W, H, 0, 0); check(PW_DIRECT_YES, W, H);
    scene(); add_above(W + PW_SHADOW_WINDOW_REACH, 0, W + 90, H, PW_WINDOW_VISIBLE, PW_SHADOW_WINDOW); check(PW_DIRECT_YES, W, H);

    /* Refusals, each with its reason; where the compositor can draw it, the screen is not the frame. */
    scene(); add_above(0, 6, W, H, PW_WINDOW_VISIBLE, 0); assert(check(PW_DIRECT_COVERED, W, H) == 0);  /* a taskbar */
    scene(); add_above(W, 0, W + 4, H, PW_WINDOW_VISIBLE, PW_SHADOW_WINDOW);                              /* its shadow */
    assert(check(PW_DIRECT_COVERED, W, H) == 0);
    scene(); s.windows[0].client_right = W - 4; s.clips[0].right = W - 4;                                 /* windowed */
    assert(check(PW_DIRECT_NOT_FULLSCREEN, W - 4, H) == 0);
    scene(); images[2].width = W / 2; images[2].height = H / 2;   /* a smaller frame would need scaling */
    assert(check(PW_DIRECT_SIZE, W / 2, H / 2) == 0);
    scene(); s.windows[0].radius = 3; assert(check(PW_DIRECT_EFFECTS, W, H) == 0);
    scene(); s.windows[0].flags |= PW_WINDOW_BORDER; s.windows[0].border = 0x0000ff; assert(check(PW_DIRECT_EFFECTS, W, H) == 0);
    scene(); s.thumbnail_count = 1;
    s.thumbnails[0] = (PwWindowThumbnail){.destination = GAME, .source = DESK, .destination_generation = 5,
        .source_generation = 1, .left = 2, .top = 2, .right = 6, .bottom = 5, .source_right = W, .source_bottom = H,
        .opacity = 255};
    desktop[0] = 0xffffffffu;  /* the thumbnail's source differs from the frame where it lands */
    assert(check(PW_DIRECT_EFFECTS, W, H) == 0);
    desktop[0] = 0xff102030u;
    scene(); s.clips[0].bottom = H - 1; assert(check(PW_DIRECT_CLIPPED, W, H) == 0);                   /* a hole */
    scene(); s.clip_count = 2; s.windows[0].clip_count = 2;                                             /* overlap */
    s.clips[0] = (PwWindowClip){0, 0, W, 5}; s.clips[1] = (PwWindowClip){0, 4, W, H - 1};
    assert((uint64_t)W * 5 + (uint64_t)W * (H - 5) == (uint64_t)W * H);  /* the same area as the screen */
    assert(check(PW_DIRECT_CLIPPED, W, H) == 0);
    scene(); s.overflow = 1; check(PW_DIRECT_INCOMPLETE, W, H);
    scene(); s.windows[0].generation = 6; assert(pw_window_direct_flip(&s, 700, GAME, 5, W, H, W, H) == PW_DIRECT_NO_WINDOW);
    scene(); s.windows[0].flags &= ~PW_WINDOW_VISIBLE; check(PW_DIRECT_NO_WINDOW, W, H);
    assert(!strcmp(pw_window_direct_reason(PW_DIRECT_COVERED), "covered") && !strcmp(pw_window_direct_reason(99), "unknown"));
    /* Game Mode off (the player's setting): the same full-screen game is composed, then flipped again once on. */
    pw_window_direct_set_enabled(0);
    scene(); assert(pw_window_direct_flip(&s, 700, GAME, 5, W, H, W, H) == PW_DIRECT_OFF);
    assert(!strcmp(pw_window_direct_reason(PW_DIRECT_OFF), "off"));
    pw_window_direct_set_enabled(1);
    scene(); check(PW_DIRECT_YES, W, H);
    /* A game's own Game Mode wins over the setting, either way, until its pid is said again (a new helper) */
    pw_window_direct_set_process(700, 0);
    scene(); assert(pw_window_direct_flip(&s, 700, GAME, 5, W, H, W, H) == PW_DIRECT_OFF);
    pw_window_direct_set_process(700, -1);
    scene(); check(PW_DIRECT_YES, W, H);
    pw_window_direct_set_enabled(0);
    pw_window_direct_set_process(700, 1);
    scene(); check(PW_DIRECT_YES, W, H);
    for (int32_t pid = 1000; pid < 1000 + PW_DIRECT_PROCESSES + 3; ++pid) pw_window_direct_set_process(pid, 0);
    pw_window_direct_set_process(700, -1);   /* gone, or overwritten: the setting again either way */
    scene(); assert(pw_window_direct_flip(&s, 700, GAME, 5, W, H, W, H) == PW_DIRECT_OFF);
    pw_window_direct_set_enabled(1);
    scene(); check(PW_DIRECT_YES, W, H);
    pw_window_effects_release(&effects);
    puts("direct flip: yes only where the compositor gives the frame itself; every refusal shown on the composed screen");
    return 0;
}
