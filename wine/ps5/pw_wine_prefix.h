/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_PREFIX_H
#define PW_WINE_PREFIX_H
/* Install the staged, runtime-owned Windows files after wineboot has made the
 * prefix and before the initial client's imports/font discovery. `source` is
 * the bundle's windows directory; `target` is the prefix's existing windows
 * directory. Each leaf folder carries .pw-proton-version, written last by the
 * existing atomic file synchronizer. A folder's .pw-retired names files the
 * runtime stopped shipping there (msyhbd.ttf, tools/stage-wine.py
 * LEFT_OUT_FONTS); each is removed from the prefix's same folder on every
 * call, by exact name and only as a regular file: no pattern, path ('/',
 * '\\', '.', '..'), directory or link (tests/test_pw_wine_prefix.c).
 * Other files/settings/saves are untouched. Missing input, an incomplete
 * wineboot or a copy failure returns -1/errno; success returns the number of
 * files copied or removed (0 on a current prefix). */
int pw_wine_prefix_prepare(const char *source, const char *target);
#endif
