/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Game Mode setting's file (src/pw_high_refresh.h): on by default, saved atomically, a bad file means on. */
#include "../src/pw_high_refresh.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char folder[] = "/tmp/proton-high-refresh-XXXXXX", path[256];
    int enabled = 0;
    assert(mkdtemp(folder));
    snprintf(path, sizeof(path), "%s/high-refresh.txt", folder);
    assert(pw_high_refresh_load(&enabled, path) == ENOENT && enabled == 1);
    assert(!pw_high_refresh_save(0, path));
    assert(!pw_high_refresh_load(&enabled, path) && enabled == 0);
    assert(!pw_high_refresh_save(1, path));
    assert(!pw_high_refresh_load(&enabled, path) && enabled == 1);
    FILE *file = fopen(path, "w");
    fputs("120hz maybe\n", file);
    fclose(file);
    assert(pw_high_refresh_load(&enabled, path) == EINVAL && enabled == 1);
    file = fopen(path, "w");
    fputs("120hz off\nextra\n", file);
    fclose(file);
    assert(pw_high_refresh_load(&enabled, path) == EINVAL && enabled == 1);
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    assert(access(tmp, F_OK) && !unlink(path) && !rmdir(folder));
    puts("120 Hz: default on, save and load, bad files");
    return 0;
}
