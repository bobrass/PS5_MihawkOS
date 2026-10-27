/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_high_refresh.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int pw_high_refresh_load(int *enabled, const char *path)
{
    char key[32], value[32], extra[32];
    int error = 0;
    *enabled = 1;
    FILE *file = fopen(path, "r");
    if (!file) return errno;
    if (fscanf(file, "%31s %31s", key, value) != 2 || strcmp(key, "120hz") ||
        (strcmp(value, "on") && strcmp(value, "off")) || fscanf(file, "%31s", extra) == 1)
        error = EINVAL;
    if (!error && ferror(file)) error = EIO;
    fclose(file);
    if (!error) *enabled = !strcmp(value, "on");
    return error;
}

int pw_high_refresh_save(int enabled, const char *path)
{
    char temporary[1024];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) return ENAMETOOLONG;
    FILE *file = fopen(temporary, "w");
    if (!file) return errno;
    int error = 0;
    if (fprintf(file, "120hz %s\n", enabled ? "on" : "off") < 0) error = EIO;
    if (fflush(file) || fsync(fileno(file))) error = errno;
    if (fclose(file) && !error) error = errno;
    if (!error && rename(temporary, path)) error = errno;
    if (error) unlink(temporary);
    return error;
}
