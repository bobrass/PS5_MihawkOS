/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_STEAMCMD_H
#define PW_STEAMCMD_H
#include <stddef.h>

/* The Steam runtime's headless client (Valve's SteamCMD, shipped by the user). The title starts
 * steamcmd.exe in the game's prefix and reads its console output, so the launcher can show the
 * runtime's real state instead of a black console window:
 *
 *   Steam Console Client (c) Valve Corporation - version 1792617903
 *   -- type 'quit' to exit --
 *   Steam>
 *   Update state (0x61) downloading, progress: 29.60 (2960 / 10000)
 *   Logging in user 'name' to Steam Public...
 *   Steam Guard code:
 *   Logged in OK
 *   FAILED (Account Logon Denied)
 *
 * This module only maps those lines to a state; the title's process wrapper owns the pipes,
 * the credentials and the Steam Guard code, which never pass through here. */

typedef enum {
    PW_STEAMCMD_IDLE = 0,    /* nothing seen yet */
    PW_STEAMCMD_UPDATE,      /* installing/updating the Steam client ("Update state ...") */
    PW_STEAMCMD_SIGNING_IN,  /* "Logging in user '...'" */
    PW_STEAMCMD_GUARD,       /* "Steam Guard code:" - the code is needed */
    PW_STEAMCMD_SIGNED_IN,   /* "Logged in OK" / "Waiting for user info...OK" */
    PW_STEAMCMD_READY,       /* the console prompt ("Steam>") is up */
    PW_STEAMCMD_FAILED,      /* "FAILED (...)" / "Login Failure" */
} PwSteamcmdState;

typedef struct PwSteamcmdStatus {
    PwSteamcmdState state;
    int percent;     /* UPDATE: 0..100, or -1 when the line carries none */
    char account[64];/* SIGNING_IN/SIGNED_IN: the account named, when the line carries it */
    char detail[160];/* the line that produced the last change, truncated */
} PwSteamcmdStatus;

void pw_steamcmd_reset(PwSteamcmdStatus *status);
/* One console line. 1 when it changed the state/percent/account/detail, 0 otherwise. */
int pw_steamcmd_line(PwSteamcmdStatus *status, const char *line);
const char *pw_steamcmd_state_name(PwSteamcmdState state);

#endif
