/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINDOW_DIRECT_H
#define PW_WINDOW_DIRECT_H
#include "pw_window_snapshot.h"
/* Game mode, step 1 (docs/PLAN.md): whether a GPU window's completed frame can go to the display as the whole
 * screen, without copying it into the scene and composing the desktop (Windows calls this independent flip).
 *
 * 2026-10-10 (record removed from the tree): composing cost 2.6 ms a frame on average and the desktop missed a refresh about once
 * or twice a second, where the launcher, which flips the game's buffer itself, never did. Flipping directly is only
 * right when composing would have produced exactly the frame, so every rule below is a case where it would not:
 *
 *   the snapshot is complete, and holds this window (pid, HWND, lifetime) as a visible GPU client;
 *   its client area is the whole screen, and the frame is screen-sized (no scaling, no offset);
 *   it is not shaped, glass or unsupported, has no rounded corner, border or thumbnail drawn on it;
 *   its visible-region clips cover the whole screen and do not overlap (nothing in Wine's view clips it);
 *   no visible window above it reaches the screen, its shadow included.
 *
 * The frame is opaque in this route (layered GPU windows are not supported), and the cursor is drawn by the
 * display in both routes, so neither is a condition. Each refusal has its own reason, for the broker's record. */
enum { PW_DIRECT_YES = 0, PW_DIRECT_INCOMPLETE, PW_DIRECT_NO_WINDOW, PW_DIRECT_NOT_FULLSCREEN, PW_DIRECT_SIZE,
       PW_DIRECT_EFFECTS, PW_DIRECT_CLIPPED, PW_DIRECT_COVERED, PW_DIRECT_OFF, PW_DIRECT_REASONS };
#ifdef __cplusplus
extern "C" {
#endif
/* The player's Game Mode setting (src/pw_game_mode.h), set by the title before the broker starts: with 0 every frame
 * is composed (reason "off"), so the broker never reports game mode and the desktop is never unloaded. On by default. */
void pw_window_direct_set_enabled(int enabled);
/* A program's own Game Mode, which wins over the setting for its windows: its library entry's WINE_PS5_GAME_MODE, or the
 * one it inherited from the program that started it (a game's launcher starts the game). pid is its GPU client's (the
 * payload pid the broker knows it by); enabled 1 on, 0 off, -1 the player's setting. The title says it for every
 * helper it starts (Wine's PW_CHILD_NOTICE_GAME_MODE), so a reused pid never keeps an ended program's choice. At most
 * PW_DIRECT_PROCESSES programs with a choice at once; one more takes the slot of the oldest. */
enum { PW_DIRECT_PROCESSES = 64 };
void pw_window_direct_set_process(int32_t pid, int enabled);
int pw_window_direct_flip(const PwWindowSnapshot *snapshot, int32_t pid, uint32_t hwnd, uint64_t generation,
                          uint32_t frame_width, uint32_t frame_height, uint32_t screen_width, uint32_t screen_height);
/* A word for a reason, for records: "yes", "incomplete", ... */
const char *pw_window_direct_reason(int reason);
#ifdef __cplusplus
}
#endif
#endif
