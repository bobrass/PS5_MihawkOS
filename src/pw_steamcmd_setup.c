/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_steamcmd_setup.h"

#include <stdio.h>
#include <string.h>

void pw_steamcmd_setup_init(PwSteamcmdSetup *setup)
{
    if (!setup) return;
    memset(setup, 0, sizeof *setup);
    setup->state = PW_STEAMCMD_SETUP_ACCOUNT;
    setup->focus = PW_STEAMCMD_SETUP_ACCOUNT;
    snprintf(setup->status, sizeof setup->status, "Sign in to Steam");
}

static int append(char *buffer, size_t capacity, int key)
{
    size_t used = strlen(buffer);
    if (key < 0x20 || key > 0x7e) return 0;
    if (used + 1 >= capacity) return 0;
    buffer[used] = (char)key;
    buffer[used + 1] = 0;
    return 1;
}

static int erase(char *buffer)
{
    size_t used = strlen(buffer);
    if (!used) return 0;
    buffer[used - 1] = 0;
    return 1;
}

int pw_steamcmd_setup_key(PwSteamcmdSetup *setup, int key)
{
    if (!setup) return 0;
    /* The pad moves between the fields; only the text fields are editable. */
    if (setup->state == PW_STEAMCMD_SETUP_ACCOUNT || setup->state == PW_STEAMCMD_SETUP_PASSWORD ||
        setup->state == PW_STEAMCMD_SETUP_GUARD) {
        if (key == PW_STEAMCMD_KEY_UP || key == PW_STEAMCMD_KEY_DOWN) {
            const int step = key == PW_STEAMCMD_KEY_UP ? -1 : 1;
            const int top = setup->state == PW_STEAMCMD_SETUP_GUARD ? PW_STEAMCMD_SETUP_GUARD : PW_STEAMCMD_SETUP_ACCOUNT;
            const int bottom = setup->state == PW_STEAMCMD_SETUP_GUARD ? PW_STEAMCMD_SETUP_GUARD : PW_STEAMCMD_SETUP_PASSWORD;
            if (setup->focus < top) setup->focus = top;
            if (setup->focus > bottom) setup->focus = bottom;
            setup->focus = setup->focus + step < top ? top : setup->focus + step > bottom ? bottom : setup->focus + step;
            setup->state = (PwSteamcmdSetupState)(setup->focus);
            setup->error = 0;
            return 1;
        }
    }
    switch (setup->state) {
    case PW_STEAMCMD_SETUP_FAILED:
        /* Any key retries: the account stays, the password does not. */
        setup->state = PW_STEAMCMD_SETUP_ACCOUNT;
        setup->focus = PW_STEAMCMD_SETUP_ACCOUNT;
        setup->error = 0;
        snprintf(setup->status, sizeof setup->status, "Sign in to Steam");
        return 1;
    case PW_STEAMCMD_SETUP_ACCOUNT:
        if (key == PW_STEAMCMD_KEY_TAB || key == PW_STEAMCMD_KEY_ENTER) {
            if (!setup->account[0]) {
                setup->error = 1;
                snprintf(setup->status, sizeof setup->status, "Enter your Steam account name");
                return 1;
            }
            setup->state = PW_STEAMCMD_SETUP_PASSWORD;
            setup->focus = PW_STEAMCMD_SETUP_PASSWORD;
            setup->error = 0;
            snprintf(setup->status, sizeof setup->status, "Now the password");
            return 1;
        }
        if (key == PW_STEAMCMD_KEY_BACKSPACE) return erase(setup->account);
        if (key == PW_STEAMCMD_KEY_ESCAPE) return 0;
        return append(setup->account, sizeof setup->account, key);
    case PW_STEAMCMD_SETUP_PASSWORD:
        if (key == PW_STEAMCMD_KEY_TAB) {
            setup->focus = PW_STEAMCMD_SETUP_ACCOUNT;
            return 1;
        }
        if (key == PW_STEAMCMD_KEY_ENTER) {
            if (!setup->password[0]) {
                setup->error = 1;
                snprintf(setup->status, sizeof setup->status, "Enter your Steam password");
                return 1;
            }
            setup->state = PW_STEAMCMD_SETUP_WAITING;
            setup->submit = 1;
            setup->error = 0;
            snprintf(setup->status, sizeof setup->status,
                 "Signing in. If Steam Guard is on, its code is on its way to your email or the Steam app.");
            return 1;
        }
        if (key == PW_STEAMCMD_KEY_BACKSPACE) return erase(setup->password);
        if (key == PW_STEAMCMD_KEY_ESCAPE) {
            setup->focus = PW_STEAMCMD_SETUP_ACCOUNT;
            return 1;
        }
        return append(setup->password, sizeof setup->password, key);
    case PW_STEAMCMD_SETUP_GUARD:
        if (key == PW_STEAMCMD_KEY_ENTER) {
            if (!setup->guard[0]) {
                setup->error = 1;
                snprintf(setup->status, sizeof setup->status,
                 "Steam Guard code requested. Check your email or the Steam mobile app, then type the code.");
                return 1;
            }
            setup->state = PW_STEAMCMD_SETUP_WAITING;
            setup->submit = 2;
            setup->error = 0;
            snprintf(setup->status, sizeof setup->status, "Checking the Steam Guard code");
            return 1;
        }
        if (key == PW_STEAMCMD_KEY_BACKSPACE) return erase(setup->guard);
        if (key == PW_STEAMCMD_KEY_ESCAPE) {
            setup->focus = PW_STEAMCMD_SETUP_ACCOUNT;
            return 1;
        }
        return append(setup->guard, sizeof setup->guard, key);
    default:
        return 0;
    }
}

int pw_steamcmd_setup_command(PwSteamcmdSetup *setup, char *out, size_t capacity)
{
    int written;
    if (!setup || !out || !capacity) return 0;
    if (setup->submit == 1) {
        setup->submit = 0;
        written = snprintf(out, capacity, "login \"%s\" \"%s\"\n", setup->account, setup->password);
    } else if (setup->submit == 2) {
        setup->submit = 0;
        written = snprintf(out, capacity, "%s\n", setup->guard);
    } else {
        return 0;
    }
    if (written < 0 || (size_t)written >= capacity) return 0;
    return 1;
}

int pw_steamcmd_setup_event(PwSteamcmdSetup *setup, const PwSteamcmdStatus *status)
{
    if (!setup || !status) return 0;
    switch (status->state) {
    case PW_STEAMCMD_UPDATE:
        setup->state = PW_STEAMCMD_SETUP_WAITING;
        setup->error = 0;
        snprintf(setup->status, sizeof setup->status, "Updating the Steam runtime");
        return 1;
    case PW_STEAMCMD_SIGNING_IN:
        setup->state = PW_STEAMCMD_SETUP_WAITING;
        setup->error = 0;
        if (status->account[0])
            snprintf(setup->status, sizeof setup->status, "Signing in as %s", status->account);
        else
            snprintf(setup->status, sizeof setup->status,
                 "Signing in. If Steam Guard is on, its code is on its way to your email or the Steam app.");
        return 1;
    case PW_STEAMCMD_GUARD:
        setup->state = PW_STEAMCMD_SETUP_GUARD;
        setup->focus = PW_STEAMCMD_SETUP_GUARD;
        setup->error = 0;
        snprintf(setup->status, sizeof setup->status,
                 "Steam Guard code requested. Check your email or the Steam mobile app, then type the code.");
        return 1;
    case PW_STEAMCMD_SIGNED_IN:
        setup->state = PW_STEAMCMD_SETUP_SIGNED_IN;
        setup->error = 0;
        memset(setup->password, 0, sizeof setup->password);
        snprintf(setup->status, sizeof setup->status, "Steam runtime ready");
        return 1;
    case PW_STEAMCMD_FAILED:
        setup->state = PW_STEAMCMD_SETUP_FAILED;
        setup->error = 1;
        memset(setup->password, 0, sizeof setup->password);
        setup->focus = PW_STEAMCMD_SETUP_ACCOUNT;
        snprintf(setup->status, sizeof setup->status, "Steam refused the sign-in");
        return 1;
    case PW_STEAMCMD_READY:
        /* The console prompt: the runtime is up and still signed out, so the fields become the screen. */
        if (setup->state == PW_STEAMCMD_SETUP_SIGNED_IN || setup->state == PW_STEAMCMD_SETUP_GUARD) return 0;
        setup->state = PW_STEAMCMD_SETUP_ACCOUNT;
        setup->focus = PW_STEAMCMD_SETUP_ACCOUNT;
        setup->error = 0;
        snprintf(setup->status, sizeof setup->status, "Sign in to Steam");
        return 1;
    default:
        return 0;
    }
}
