/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_HIGH_REFRESH_H
#define PW_HIGH_REFRESH_H
/* The player's 120 Hz setting (MihawkOS's Settings and its WebUI, saved beside game-mode.txt as high-refresh.txt):
 * one choice for the whole console, as the display's rate is the title's once per session.
 *
 * On (the default): every session asks for 120 Hz output, PROTON_120HZ=1 (src/pw_speed_defaults.h), where the TV
 * takes it. Off: the session sets PROTON_120HZ=0 and the display stays at 60 Hz, for a TV that shows no picture or
 * flickers at 120 Hz. A game's own PROTON_120HZ in its game.json "env" still wins (a game that counts vblanks for its
 * timing). The file holds one line, "120hz on" or "120hz off"; a missing or unreadable file means on. Save is atomic
 * (a temporary file, fsync, rename): a failed save leaves the previous file. Functions return errno values. */
#ifdef __cplusplus
extern "C" {
#endif
/* *enabled: 1 or 0; 1 when the file is missing (ENOENT) or not understood (EINVAL). */
int pw_high_refresh_load(int *enabled, const char *path);
int pw_high_refresh_save(int enabled, const char *path);
#ifdef __cplusplus
}
#endif
#endif
