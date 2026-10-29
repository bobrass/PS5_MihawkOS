/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The warm helper's rules (src/pw_warm_helper.h): one parked and one being made at most, none after spawns end or when
 * switched off, taken once, closed PW_WARM_KEEP_MS after the last start. */
#include "../src/pw_warm_helper.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    PwWarmHelper warm = PW_WARM_HELPER_INIT;
    int fd = 0, pid = 0;

    assert(pw_warm_enabled(NULL) && pw_warm_enabled("1") && pw_warm_enabled("") && !pw_warm_enabled("0"));
    assert(!pw_warm_should_make(&warm, 0, 0) && !pw_warm_should_make(&warm, 1, 1));
    assert(pw_warm_should_make(&warm, 1, 0));
    assert(!pw_warm_take(&warm, &fd, &pid));

    /* Being made: no second one; parked: none made, then taken once. */
    warm.making = 1;
    assert(!pw_warm_should_make(&warm, 1, 0));
    warm.last_start_ms = 1000;
    assert(pw_warm_park(&warm, 7, 70, 0) && !warm.making);
    assert(!pw_warm_should_make(&warm, 1, 0));
    assert(pw_warm_wait_ms(&warm, 70, 1000, 0) == PW_WARM_KEEP_MS);
    assert(pw_warm_wait_ms(&warm, 70, 1000 + PW_WARM_KEEP_MS - 1, 0) == 1);
    assert(pw_warm_wait_ms(&warm, 70, 1000 + PW_WARM_KEEP_MS, 0) == 0);
    assert(pw_warm_wait_ms(&warm, 70, 1000, 1) == 0);        /* spawns ended: close it now */
    warm.last_start_ms = 5000;                                /* a start restarts the keep */
    assert(pw_warm_wait_ms(&warm, 70, 1000 + PW_WARM_KEEP_MS, 0) == 4000);
    assert(pw_warm_take(&warm, &fd, &pid) && fd == 7 && pid == 70);
    assert(!pw_warm_take(&warm, &fd, &pid));
    assert(pw_warm_wait_ms(&warm, 70, 5000, 0) == -1);        /* taken: its keeper stops */

    /* A second one ready while one is parked, or after spawns ended, or not a process: the caller closes it. */
    warm.making = 1;
    assert(pw_warm_park(&warm, 8, 80, 0));
    warm.making = 1;
    assert(!pw_warm_park(&warm, 9, 90, 0) && !warm.making && warm.pid == 80);
    assert(pw_warm_wait_ms(&warm, 90, 5000, 0) == -1);        /* not the parked one */
    assert(pw_warm_take(&warm, &fd, &pid) && pid == 80);
    assert(!pw_warm_park(&warm, 9, 90, 1) && warm.fd < 0);
    assert(!pw_warm_park(&warm, -1, 90, 0) && !pw_warm_park(&warm, 9, 0, 0));
    assert(PW_WARM_BOOTSTRAP_MS > PW_WARM_KEEP_MS);

    printf("warm helper passed: one parked, one made, taken once, kept %d ms after the last start\n", PW_WARM_KEEP_MS);
    return 0;
}
