/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/pw_path_nocase.h on a real folder: an exact name, other cases, a folder in the middle, a miss, a short buffer. */
#include "../src/pw_path_nocase.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void touch(const char *path)
{
    FILE *file = fopen(path, "w");
    assert(file);
    fclose(file);
}

int main(void)
{
    char root[] = "/tmp/pw-nocase-XXXXXX", path[512], out[512], expect[512];
    assert(mkdtemp(root));
    snprintf(path, sizeof(path), "%s/Program Files (x86)", root);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/Program Files (x86)/Vendor", root);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/Program Files (x86)/Vendor/Client.exe", root);
    touch(path);

    snprintf(expect, sizeof(expect), "%s", path);
    assert(pw_path_nocase(path, out, sizeof(out)) == 1 && !strcmp(out, expect));          /* exact */
    snprintf(path, sizeof(path), "%s/Program Files (x86)/Vendor/client.exe", root);
    assert(pw_path_nocase(path, out, sizeof(out)) == 1 && !strcmp(out, expect));          /* the file's case */
    snprintf(path, sizeof(path), "%s/program files (X86)/VENDOR/CLIENT.EXE", root);
    assert(pw_path_nocase(path, out, sizeof(out)) == 1 && !strcmp(out, expect));          /* every component's */
    snprintf(path, sizeof(path), "%s/Program Files (x86)/Vendor/nothing.exe", root);
    assert(pw_path_nocase(path, out, sizeof(out)) == 0);                                  /* a miss */
    snprintf(path, sizeof(path), "%s/Program Files (x86)/Vendor/client.exe", root);
    assert(pw_path_nocase(path, out, 8) == 0);                                            /* a buffer too small */
    assert(pw_path_nocase("relative/client.exe", out, sizeof(out)) == 0);
    assert(pw_path_nocase(NULL, out, sizeof(out)) == 0 && pw_path_nocase(path, NULL, 4) == 0 && pw_path_nocase(path, out, 0) == 0);
    assert(pw_path_nocase("/", out, sizeof(out)) == 1 && !strcmp(out, "/"));

    snprintf(path, sizeof(path), "rm -rf '%s'", root);
    assert(system(path) == 0);
    puts("test_pw_path_nocase: ok");
    return 0;
}
