/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_controller.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *const words[PW_CONTROLLER_KINDS] = {"xbox-360", "xbox-one", "dualsense"};
static const char *const names[PW_CONTROLLER_KINDS] = {"Xbox 360 controller", "Xbox One controller", "DualSense"};

const char *pw_controller_word(int kind)
{
    return kind >= 0 && kind < PW_CONTROLLER_KINDS ? words[kind] : NULL;
}

int pw_controller_kind(const char *word)
{
    for (int k = 0; word && k < PW_CONTROLLER_KINDS; ++k)
        if (!strcmp(word, words[k])) return k;
    return -1;
}

const char *pw_controller_name(int kind)
{
    return kind >= 0 && kind < PW_CONTROLLER_KINDS ? names[kind] : NULL;
}

int pw_controller_load(int *kind, const char *path)
{
    char key[32], value[32], extra[32];
    int error = EINVAL, found = -1;
    *kind = PW_CONTROLLER_XBOX_360;
    FILE *file = fopen(path, "r");
    if (!file) return errno;
    if (fscanf(file, "%31s %31s", key, value) == 2 && !strcmp(key, "controller") && fscanf(file, "%31s", extra) != 1 &&
        (found = pw_controller_kind(value)) >= 0)
        error = 0;
    if (!error && ferror(file)) error = EIO;
    fclose(file);
    if (!error) *kind = found;
    return error;
}

int pw_controller_save(int kind, const char *path)
{
    if (!pw_controller_word(kind)) return EINVAL;
    char temporary[1024];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) return ENAMETOOLONG;
    FILE *file = fopen(temporary, "w");
    if (!file) return errno;
    int error = 0;
    if (fprintf(file, "controller %s\n", words[kind]) < 0) error = EIO;
    if (fflush(file) || fsync(fileno(file))) error = errno;
    if (fclose(file) && !error) error = errno;
    if (!error && rename(temporary, path)) error = errno;
    if (error) unlink(temporary);
    return error;
}
