/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_GAME_MODE_H
#define PW_GAME_MODE_H
/* The player's Game Mode setting (MihawkOS's Settings, saved beside controls.txt as game-mode.txt).
 *
 * On (the default): a full-screen game's frames go to the display directly (src/pw_window_direct.h) and the Mihawk
 * desktop is unloaded while it holds the screen (src/pw_desktop_session.h). Off: every frame is composed into the
 * desktop and the desktop stays loaded; the player asked for it (2026-10-10) to rule game mode out when the screen
 * goes black. The file holds one line, "game-mode on" or "game-mode off"; a missing or unreadable file means on.
 * Save is atomic (a temporary file, fsync, rename): a failed save leaves the previous file. Functions return errno
 * values. */
/* A game's own choice (game.json "env", or a launch's): "1" on, "0" off, anything else the player's setting. It wins
 * over the file for the game's session (native/proton_wine.cpp) and for its windows in the Mihawk OS session, where
 * the programs it starts inherit it (src/pw_window_direct.h). */
#define PW_GAME_MODE_ENV "WINE_PS5_GAME_MODE"
#ifdef __cplusplus
extern "C" {
#endif
/* *enabled: 1 or 0; 1 when the file is missing (ENOENT) or not understood (EINVAL). */
int pw_game_mode_load(int *enabled, const char *path);
int pw_game_mode_save(int enabled, const char *path);
#ifdef __cplusplus
}
#endif
#endif
