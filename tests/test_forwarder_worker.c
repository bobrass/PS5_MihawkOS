/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What the tiles' worker (forwarder/worker.c) reads in ShadowMountPlus's answers (forwarder/forwarder.h). */
#include "../forwarder/forwarder.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(forwarder_reply_status("{\"status\":0,\"games\":[]}") == 0);
    assert(forwarder_reply_status("{ \"status\" : 16, \"error\":\"busy\", \"error_reason\":\"game active\" }") == 16);
    assert(forwarder_reply_status("{\"status\":-1}") == -1 && forwarder_reply_status("{\"games\":[]}") == -1);
    assert(forwarder_reply_status(NULL) == -1 && forwarder_reply_status("{\"status\":\"x\"}") == -1);
    const char *games = "{\"status\":0,\"games\":[{\"title_id\":\"PPSA99300\",\"installed\":true},"
                        "{\"installed\":true,\"title_id\":\"PPSA99301\"},{\"title_id\":\"PPSA99302\",\"installed\":false},"
                        "{\"title_id\":\"PPSA99303\"},{\"installed\": true}]}";
    assert(forwarder_reply_installed(games, "PPSA99300") && forwarder_reply_installed(games, "PPSA99301"));
    assert(!forwarder_reply_installed(games, "PPSA99302") && !forwarder_reply_installed(games, "PPSA99303"));
    assert(!forwarder_reply_installed(games, "PPSA99304") && !forwarder_reply_installed(NULL, "PPSA99300"));
    puts("forwarder worker: ShadowMountPlus's status and installed titles read");
    return 0;
}
