/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_path_nocase.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

int pw_path_nocase(const char *path, char *out, size_t cap)
{
    struct stat status;
    if (!path || !out || !cap || path[0] != '/') return 0;
    if (strlen(path) >= cap) return 0;
    if (stat(path, &status) == 0) {
        memcpy(out, path, strlen(path) + 1);
        return 1;
    }
    size_t used = 0;
    out[0] = 0;
    for (const char *at = path; *at;) {
        while (*at == '/') at++;
        if (!*at) break;
        const char *end = strchr(at, '/');
        const size_t length = end ? (size_t)(end - at) : strlen(at);
        char name[256];
        if (length >= sizeof(name)) return 0;
        memcpy(name, at, length);
        name[length] = 0;
        at += length;
        char next[4096];
        const int wrote = snprintf(next, sizeof(next), "%s/%s", out, name);
        if (wrote < 0 || (size_t)wrote >= sizeof(next)) return 0;
        if (stat(next, &status) != 0) {
            DIR *folder = opendir(used ? out : "/");
            int found = 0;
            if (!folder) return 0;
            for (struct dirent *entry; !found && (entry = readdir(folder));) {
                if (strcasecmp(entry->d_name, name) != 0) continue;
                found = snprintf(next, sizeof(next), "%s/%s", out, entry->d_name) < (int)sizeof(next);
            }
            closedir(folder);
            if (!found) return 0;
        }
        used = strlen(next);
        if (used >= cap) return 0;
        memcpy(out, next, used + 1);
    }
    return used != 0;
}
