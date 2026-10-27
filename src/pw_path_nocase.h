/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_PATH_NOCASE_H
#define PW_PATH_NOCASE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wine matches file names without regard to case; the console's file system and the launcher's own checks do not. A
 * program that an installer wrote as "Client.exe" is found by Windows as "client.exe" and by stat() not at all.
 *
 * Resolves `path` (absolute, '/' separators) to the name the file system holds: the exact path when it exists, else
 * each component matched against its folder's entries ignoring case (the first match in the folder's order, so a folder
 * holding two names that differ only in case answers either). Returns 1 with the path in `out` (cap bytes), 0 when
 * nothing matches or `out` is too small. Costs one stat when the path is exact; a folder scan per component otherwise. */
int pw_path_nocase(const char *path, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
