/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The extended-attribute store of Wine's Unix side (wine/ps5/pw_wine_xattr.h), on a scratch folder. */
#include "../wine/ps5/pw_wine_xattr.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ENODATA
#define ENODATA ENOATTR
#endif

int main(void)
{
    char dir[] = "/tmp/pw-xattr-XXXXXX", store[64], buffer[32];
    assert(mkdtemp(dir));
    snprintf(store, sizeof(store), "%s/store", dir);

    errno = 0;
    assert(pw_wine_xattr_get(1, 2, "user.DOSATTRIB", buffer, sizeof(buffer)) == -1 && errno == ENOTSUP);
    assert(!pw_wine_xattr_init(store));

    /* none yet: ENODATA, as ntdll expects of a file without attributes */
    errno = 0;
    assert(pw_wine_xattr_get(1, 2, "user.DOSATTRIB", buffer, sizeof(buffer)) == -1 && errno == ENODATA);
    assert(pw_wine_xattr_remove(1, 2, "user.DOSATTRIB") == -1 && errno == ENODATA);

    /* set, read back, by inode: another inode and another name see nothing */
    assert(!pw_wine_xattr_set(1, 2, "user.DOSATTRIB", "0x2", 3));
    memset(buffer, 0, sizeof(buffer));
    assert(pw_wine_xattr_get(1, 2, "user.DOSATTRIB", buffer, sizeof(buffer)) == 3 && !memcmp(buffer, "0x2", 3));
    assert(pw_wine_xattr_get(1, 2, "user.DOSATTRIB", NULL, 0) == 3);
    errno = 0;
    assert(pw_wine_xattr_get(1, 2, "user.DOSATTRIB", buffer, 2) == -1 && errno == ERANGE);
    errno = 0;
    assert(pw_wine_xattr_get(1, 3, "user.DOSATTRIB", buffer, sizeof(buffer)) == -1 && errno == ENODATA);
    errno = 0;
    assert(pw_wine_xattr_get(1, 2, "user.WINEREPARSE", buffer, sizeof(buffer)) == -1 && errno == ENODATA);

    /* replaced whole */
    assert(!pw_wine_xattr_set(1, 2, "user.DOSATTRIB", "0x6", 3));
    assert(pw_wine_xattr_get(1, 2, "user.DOSATTRIB", buffer, sizeof(buffer)) == 3 && !memcmp(buffer, "0x6", 3));

    /* a second name, then a remove of one keeps the other */
    assert(!pw_wine_xattr_set(1, 2, "user.WINEREPARSE", "\x03\x00\x00\xa0", 4));
    assert(!pw_wine_xattr_remove(1, 2, "user.DOSATTRIB"));
    errno = 0;
    assert(pw_wine_xattr_get(1, 2, "user.DOSATTRIB", buffer, sizeof(buffer)) == -1 && errno == ENODATA);
    assert(pw_wine_xattr_get(1, 2, "user.WINEREPARSE", buffer, sizeof(buffer)) == 4);

    /* a value another process wrote is seen: the store is read again while it changes */
    char other[128];
    snprintf(other, sizeof(other), "%s/%llx-%llx.user.DOSATTRIB", store, 5ULL, 9ULL);
    FILE *f = fopen(other, "w");
    assert(f && fputs("0x4", f) >= 0 && !fclose(f));
    assert(pw_wine_xattr_get(5, 9, "user.DOSATTRIB", buffer, sizeof(buffer)) == 3 && !memcmp(buffer, "0x4", 3));

    /* a new file on a reused inode starts empty */
    pw_wine_xattr_forget(1, 2);
    errno = 0;
    assert(pw_wine_xattr_get(1, 2, "user.WINEREPARSE", buffer, sizeof(buffer)) == -1 && errno == ENODATA);
    pw_wine_xattr_forget(7, 7);   /* nothing to forget: no effect */
    assert(pw_wine_xattr_get(5, 9, "user.DOSATTRIB", buffer, sizeof(buffer)) == 3);

    /* names cannot leave the store */
    errno = 0;
    assert(pw_wine_xattr_set(1, 2, "user/../../x", "y", 1) == -1 && errno == EINVAL);

    /* no temporary is left behind */
    char command[160];
    snprintf(command, sizeof(command), "ls -a %s | grep -q '^\\.new-'", store);
    assert(system(command) != 0);

    snprintf(command, sizeof(command), "rm -rf %s", dir);
    assert(!system(command));
    puts("test_pw_wine_xattr: ok");
    return 0;
}
