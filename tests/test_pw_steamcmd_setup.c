/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_steamcmd_setup.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void type_text(PwSteamcmdSetup *setup, const char *text)
{
    for (const char *at = text; *at; ++at) pw_steamcmd_setup_key(setup, (unsigned char)*at);
}

int main(void)
{
    PwSteamcmdSetup setup;
    PwSteamcmdStatus status;
    char command[256];

    pw_steamcmd_setup_init(&setup);
    assert(setup.state == PW_STEAMCMD_SETUP_ACCOUNT);

    type_text(&setup, "mihawk");
    assert(strcmp(setup.account, "mihawk") == 0);
    pw_steamcmd_setup_key(&setup, PW_STEAMCMD_KEY_TAB);
    assert(setup.state == PW_STEAMCMD_SETUP_PASSWORD && setup.focus == PW_STEAMCMD_SETUP_PASSWORD);
    type_text(&setup, "secret");
    assert(pw_steamcmd_setup_key(&setup, PW_STEAMCMD_KEY_ENTER) == 1);
    assert(setup.state == PW_STEAMCMD_SETUP_WAITING);
    assert(pw_steamcmd_setup_command(&setup, command, sizeof command) == 1);
    assert(strcmp(command, "login \"mihawk\" \"secret\"\n") == 0);
    assert(pw_steamcmd_setup_command(&setup, command, sizeof command) == 0);

    pw_steamcmd_reset(&status);
    status.state = PW_STEAMCMD_GUARD;
    assert(pw_steamcmd_setup_event(&setup, &status) == 1);
    assert(setup.state == PW_STEAMCMD_SETUP_GUARD && setup.focus == PW_STEAMCMD_SETUP_GUARD);
    type_text(&setup, "ABCDE");
    pw_steamcmd_setup_key(&setup, PW_STEAMCMD_KEY_ENTER);
    assert(pw_steamcmd_setup_command(&setup, command, sizeof command) == 1);
    assert(strcmp(command, "ABCDE\n") == 0);

    status.state = PW_STEAMCMD_SIGNED_IN;
    assert(pw_steamcmd_setup_event(&setup, &status) == 1);
    assert(setup.state == PW_STEAMCMD_SETUP_SIGNED_IN);
    assert(setup.password[0] == 0);

    pw_steamcmd_setup_init(&setup);
    pw_steamcmd_reset(&status);
    status.state = PW_STEAMCMD_READY;
    assert(pw_steamcmd_setup_event(&setup, &status) == 1);
    assert(setup.state == PW_STEAMCMD_SETUP_ACCOUNT);
    assert(strcmp(setup.status, "Sign in to Steam") == 0);

    pw_steamcmd_setup_init(&setup);
    status.state = PW_STEAMCMD_FAILED;
    assert(pw_steamcmd_setup_event(&setup, &status) == 1);
    assert(setup.state == PW_STEAMCMD_SETUP_FAILED && setup.error == 1);
    pw_steamcmd_setup_key(&setup, 'a');
    assert(setup.state == PW_STEAMCMD_SETUP_ACCOUNT && setup.error == 0);

    pw_steamcmd_setup_init(&setup);
    type_text(&setup, "mihawk");
    pw_steamcmd_setup_key(&setup, PW_STEAMCMD_KEY_ENTER);
    assert(setup.state == PW_STEAMCMD_SETUP_PASSWORD);
    pw_steamcmd_setup_key(&setup, PW_STEAMCMD_KEY_ENTER);
    assert(setup.state == PW_STEAMCMD_SETUP_PASSWORD && setup.error == 1);

    puts("test_pw_steamcmd_setup: ok");
    return 0;
}
