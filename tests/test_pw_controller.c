/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The controller setting's file (src/pw_controller.h): Xbox 360 by default, each choice saved and read back, a bad file
 * means the default. */
#include "../src/pw_controller.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char folder[] = "/tmp/proton-controller-XXXXXX", path[256];
    int kind = -1;
    assert(mkdtemp(folder));
    snprintf(path, sizeof(path), "%s/controller.txt", folder);
    assert(pw_controller_load(&kind, path) == ENOENT && kind == PW_CONTROLLER_XBOX_360);
    for (int k = 0; k < PW_CONTROLLER_KINDS; ++k) {
        assert(!pw_controller_save(k, path));
        assert(!pw_controller_load(&kind, path) && kind == k);
        assert(pw_controller_word(k) && pw_controller_name(k));
    }
    assert(!strcmp(pw_controller_word(PW_CONTROLLER_DUALSENSE), "dualsense") && !pw_controller_word(PW_CONTROLLER_KINDS));
    assert(pw_controller_kind("xbox-one") == PW_CONTROLLER_XBOX_ONE && pw_controller_kind("Xbox") == -1 && pw_controller_kind(NULL) == -1);
    assert(pw_controller_save(PW_CONTROLLER_KINDS, path) == EINVAL);
    assert(!pw_controller_load(&kind, path) && kind == PW_CONTROLLER_DUALSENSE);   /* the refused save left the file */
    FILE *file = fopen(path, "w");
    fputs("controller xbox-series\n", file);
    fclose(file);
    assert(pw_controller_load(&kind, path) == EINVAL && kind == PW_CONTROLLER_XBOX_360);
    file = fopen(path, "w");
    fputs("controller dualsense\nextra\n", file);
    fclose(file);
    assert(pw_controller_load(&kind, path) == EINVAL && kind == PW_CONTROLLER_XBOX_360);
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    assert(access(tmp, F_OK) && !unlink(path) && !rmdir(folder));
    puts("controller: default Xbox 360, each choice saved and loaded, bad files");
    return 0;
}
