/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_prefix.h"
#include "../../src/pw_wine_launch.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Removes from `target` each name listed in `source`/.pw-retired (one plain file name per line): files the runtime
 * once put there and stopped shipping. Only that exact name in that folder, and only a regular file: no pattern, no
 * path, no directory or link. A name that is absent is already gone; any other failure is the caller's. */
static int retire_files(const char *source, const char *target)
{
    char path[1024], name[256];
    struct stat state;
    int removed = 0;
    if (snprintf(path, sizeof(path), "%s/.pw-retired", source) >= (int)sizeof(path)) { errno = ENAMETOOLONG; return -1; }
    FILE *list = fopen(path, "r");
    if (!list) return -1;
    while (fgets(name, sizeof(name), list)) {
        name[strcspn(name, "\r\n")] = 0;
        if (!name[0] || strchr(name, '/') || strchr(name, '\\') || !strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (snprintf(path, sizeof(path), "%s/%s", target, name) >= (int)sizeof(path)) continue;
        if (lstat(path, &state)) {
            if (errno == ENOENT) continue;
            int saved = errno; fclose(list); errno = saved; return -1;
        }
        if (!S_ISREG(state.st_mode)) continue;
        if (!unlink(path)) ++removed;
        else if (errno != ENOENT) { int saved = errno; fclose(list); errno = saved; return -1; }
    }
    fclose(list);
    return removed;
}

static int prepare_folder(const char *source, const char *target, unsigned depth)
{
    enum { MAX_FILES = 128 };
    char from[1024], to[1024];
    const char *names[MAX_FILES];
    size_t count = 0;
    int copied = 0, failed = 0, saved = 0, retired = 0;
    struct stat state;
    if (depth > 4) { errno = ELOOP; return -1; }
    DIR *dir = opendir(source);
    if (!dir) return -1;
    if (stat(target, &state) || !S_ISDIR(state.st_mode)) {
        closedir(dir); errno = ENOTDIR; return -1;
    }
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) { failed = 1; saved = errno; } break; }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".pw-proton-version")) continue;
        if (!strcmp(name, ".pw-retired")) { retired = 1; continue; }
        int a = snprintf(from, sizeof(from), "%s/%s", source, name);
        int b = snprintf(to, sizeof(to), "%s/%s", target, name);
        if (a < 0 || b < 0 || (size_t)a >= sizeof(from) || (size_t)b >= sizeof(to)) {
            failed = 1; saved = ENAMETOOLONG; break;
        }
        if (lstat(from, &state)) { failed = 1; saved = errno; break; }
        if (S_ISDIR(state.st_mode)) {
            if (mkdir(to, 0755) && errno != EEXIST) { failed = 1; saved = errno; break; }
            int n = prepare_folder(from, to, depth + 1);
            if (n < 0) { failed = 1; saved = errno; break; }
            copied += n;
        } else if (S_ISREG(state.st_mode)) {
            if (count == MAX_FILES) { failed = 1; saved = E2BIG; break; }
            names[count] = strdup(name);
            if (!names[count]) { failed = 1; saved = ENOMEM; break; }
            ++count;
        } else { failed = 1; saved = EINVAL; break; }
    }
    closedir(dir);
    if (!failed && retired) {
        int n = retire_files(source, target);
        if (n < 0) { failed = 1; saved = errno; } else copied += n;
    }
    if (!failed && count) {
        int n = pw_wine_launch_sync(source, target, names, count, ".pw-proton-version");
        if (n < 0) { failed = 1; saved = errno; } else copied += n;
    }
    for (size_t i = 0; i < count; ++i) free((void *)names[i]);
    if (failed) { errno = saved; return -1; }
    return copied;
}

int pw_wine_prefix_prepare(const char *source, const char *target)
{
    if (!source || !target) { errno = EINVAL; return -1; }
    return prepare_folder(source, target, 0);
}
