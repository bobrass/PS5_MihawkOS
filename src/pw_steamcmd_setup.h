/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_STEAMCMD_SETUP_H
#define PW_STEAMCMD_SETUP_H
#include "pw_steamcmd.h"
#include <stddef.h>

/* The Steam runtime's first-run page (docs/aegis/plans/2026-10-23-steamcmd-runtime.md, slice 2): the account and
 * password typed on the TV with the keyboard, then the Steam Guard code; the title writes the login commands to
 * SteamCMD's stdin. The password is never stored; the SteamCMD's own machine token (ssfn) is what persists. */

typedef enum {
    PW_STEAMCMD_SETUP_ACCOUNT = 0, /* typing the account name */
    PW_STEAMCMD_SETUP_PASSWORD,    /* typing the password (masked) */
    PW_STEAMCMD_SETUP_GUARD,       /* SteamCMD asked for the Steam Guard code */
    PW_STEAMCMD_SETUP_WAITING,     /* the login is in flight */
    PW_STEAMCMD_SETUP_FAILED,      /* SteamCMD refused; the fields keep the account, not the password */
    PW_STEAMCMD_SETUP_SIGNED_IN,   /* the runtime is ready */
} PwSteamcmdSetupState;

typedef struct PwSteamcmdSetup {
    PwSteamcmdSetupState state;
    char account[64];
    char password[128];
    char guard[16];
    int focus;          /* the field the keyboard edits: ACCOUNT, PASSWORD, GUARD */
    int error;          /* draw the status in the danger colour */
    int submit;         /* 1: send the login line, 2: send the Guard line, on the next command() */
    char status[160];   /* the last line shown under the fields */
} PwSteamcmdSetup;

void pw_steamcmd_setup_init(PwSteamcmdSetup *setup);
/* One typed character (a printable ASCII byte) or an editing key. The keys below are the setup's own, so the caller
 * maps the console's virtual keys once. 1 when the page changed. */
enum {
    PW_STEAMCMD_KEY_TAB = 0x100, PW_STEAMCMD_KEY_ENTER, PW_STEAMCMD_KEY_BACKSPACE, PW_STEAMCMD_KEY_ESCAPE,
    PW_STEAMCMD_KEY_UP, PW_STEAMCMD_KEY_DOWN,
};
int pw_steamcmd_setup_key(PwSteamcmdSetup *setup, int key);
/* The next line for SteamCMD's stdin, newline included. 1 when written; 0 when there is nothing to send; the caller
 * writes it and keeps the write end of the pipe. The password is copied here only for that one write. */
int pw_steamcmd_setup_command(PwSteamcmdSetup *setup, char *out, size_t capacity);
/* SteamCMD's parsed console state -> the page (signing in, Guard, refused, signed in). */
int pw_steamcmd_setup_event(PwSteamcmdSetup *setup, const PwSteamcmdStatus *status);

#endif
