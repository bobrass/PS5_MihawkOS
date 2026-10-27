/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_steamcmd.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void copy_text(char *dst, size_t cap, const char *src, size_t len)
{
    if (!cap) return;
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
    /* SteamCMD writes CRLF; keep the parser's detail one line. */
    while (len && (dst[len - 1] == '\r' || dst[len - 1] == '\n')) dst[--len] = 0;
}

void pw_steamcmd_reset(PwSteamcmdStatus *status)
{
    if (!status) return;
    memset(status, 0, sizeof *status);
    status->state = PW_STEAMCMD_IDLE;
    status->percent = -1;
}

const char *pw_steamcmd_state_name(PwSteamcmdState state)
{
    switch (state) {
    case PW_STEAMCMD_IDLE: return "idle";
    case PW_STEAMCMD_UPDATE: return "update";
    case PW_STEAMCMD_SIGNING_IN: return "signing-in";
    case PW_STEAMCMD_GUARD: return "guard";
    case PW_STEAMCMD_SIGNED_IN: return "signed-in";
    case PW_STEAMCMD_READY: return "ready";
    case PW_STEAMCMD_FAILED: return "failed";
    }
    return "idle";
}

static int update_percent(const char *line)
{
    const char *at = strstr(line, "progress:");
    int whole = 0;
    if (!at) return -1;
    at += strlen("progress:");
    while (*at == ' ' || *at == '\t') ++at;
    if (!isdigit((unsigned char)*at)) return -1;
    while (isdigit((unsigned char)*at)) {
        if (whole < 1000) whole = whole * 10 + (*at - '0');
        ++at;
    }
    return whole > 100 ? 100 : whole;
}

static void account_name(const char *line, char *dst, size_t cap)
{
    const char *open = strchr(line, '\'');
    const char *close;
    if (!open) return;
    close = strchr(open + 1, '\'');
    if (!close || close == open + 1) return;
    copy_text(dst, cap, open + 1, (size_t)(close - open - 1));
}

int pw_steamcmd_line(PwSteamcmdStatus *status, const char *line)
{
    PwSteamcmdState state;
    const char *detail = line;
    int percent = -1;
    int changed;

    if (!status || !line) return 0;
    if (!*line) return 0;

    /* SteamCMD logs the real sign-in verdict to connection_log.txt; its interactive console text is not captured. */
    if (strstr(line, "Logged On")) {
        state = PW_STEAMCMD_SIGNED_IN;
    } else if (strstr(line, "Invalid Password") || strstr(line, "Account Logon Denied") ||
               strstr(line, "InvalidLoginAuthCode") || strstr(line, "Rate Limit")) {
        state = PW_STEAMCMD_FAILED;
    } else if (strstr(line, "Steam Guard")) {
        state = PW_STEAMCMD_GUARD;
    } else if (strstr(line, "Logging on") || strstr(line, "BeginAuthSessionViaCredentials")) {
        state = PW_STEAMCMD_SIGNING_IN;
    } else if (strstr(line, "Update state")) {
        state = PW_STEAMCMD_UPDATE;
        percent = update_percent(line);
    } else if (strstr(line, "Steam Guard code")) {
        state = PW_STEAMCMD_GUARD;
    } else if (strstr(line, "FAILED (") || strstr(line, "Login Failure")) {
        state = PW_STEAMCMD_FAILED;
    } else if (strstr(line, "Logged in OK") || strstr(line, "Waiting for user info...OK")) {
        state = PW_STEAMCMD_SIGNED_IN;
    } else if (strstr(line, "Logging in user")) {
        state = PW_STEAMCMD_SIGNING_IN;
    } else if (strstr(line, "Steam Console Client") || strstr(line, "-- type 'quit' to exit --") ||
               strstr(line, "Loading Steam API...OK") || strncmp(line, "Steam>", 6) == 0) {
        state = PW_STEAMCMD_READY;
    } else {
        return 0;
    }

    changed = state != status->state || (percent >= 0 && percent != status->percent) ||
              strcmp(detail, status->detail) != 0;
    status->state = state;
    if (percent >= 0) status->percent = percent;
    copy_text(status->detail, sizeof status->detail, detail, strlen(detail));
    if (state == PW_STEAMCMD_SIGNING_IN || state == PW_STEAMCMD_SIGNED_IN) {
        char account[sizeof status->account] = {};
        account_name(line, account, sizeof account);
        if (account[0] && strcmp(account, status->account) != 0) {
            copy_text(status->account, sizeof status->account, account, strlen(account));
            changed = 1;
        }
    }
    return changed;
}
