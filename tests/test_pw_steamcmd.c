/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The lines SteamCMD printed in the mihawk prefix (klog/steamcmd-login.log, 2026-10-23) and the
 * state the title's overlay derives from them. */
#include "../src/pw_steamcmd.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect(PwSteamcmdStatus *status, const char *line, PwSteamcmdState state)
{
    pw_steamcmd_line(status, line);
    if (status->state != state) {
        fprintf(stderr, "line %.80s gave %s, expected %s\n", line, pw_steamcmd_state_name(status->state),
                pw_steamcmd_state_name(state));
        assert(0);
    }
}

int main(void)
{
    PwSteamcmdStatus status;

    pw_steamcmd_reset(&status);
    assert(status.state == PW_STEAMCMD_IDLE);
    assert(status.percent == -1);
    assert(strcmp(pw_steamcmd_state_name(PW_STEAMCMD_GUARD), "guard") == 0);

    expect(&status, "Steam Console Client (c) Valve Corporation - version 1792617903", PW_STEAMCMD_READY);
    expect(&status, "-- type 'quit' to exit --", PW_STEAMCMD_READY);
    expect(&status, "Steam>", PW_STEAMCMD_READY);
    expect(&status, "Update state (0x61) downloading, progress: 29.60 (2960 / 10000)", PW_STEAMCMD_UPDATE);
    assert(status.percent == 29);
    expect(&status, "Logging in user 'mihawk' to Steam Public...", PW_STEAMCMD_SIGNING_IN);
    assert(strcmp(status.account, "mihawk") == 0);
    expect(&status, "Steam Guard code:", PW_STEAMCMD_GUARD);
    expect(&status, "Logged in OK", PW_STEAMCMD_SIGNED_IN);
    expect(&status, "Waiting for user info...OK", PW_STEAMCMD_SIGNED_IN);
    expect(&status, "Loading Steam API...OK", PW_STEAMCMD_READY);
    expect(&status, "Update state (0x65) downloading, progress: 100.00 (100 / 100)", PW_STEAMCMD_UPDATE);
    assert(status.percent == 100);

    pw_steamcmd_reset(&status);
    expect(&status, "FAILED (Account Logon Denied)\r", PW_STEAMCMD_FAILED);
    assert(strstr(status.detail, "FAILED (Account Logon Denied)") != NULL);
    assert(status.detail[strlen(status.detail) - 1] != '\r');

    pw_steamcmd_reset(&status);
    assert(pw_steamcmd_line(&status, "[  0%] Downloading item 123...") == 0);
    assert(pw_steamcmd_line(&status, "") == 0);
    assert(status.state == PW_STEAMCMD_IDLE);

    puts("test_pw_steamcmd: ok");
    return 0;
}
