/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Steam guide's shared rules (pw_steam_guide.h). */
#include "pw_steam_guide.h"
#include "pw_https.h"
#include "pw_setup.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

PwSteamFailure pw_steam_guide_fetch_failure(int https_status)
{
    switch (https_status) {
    case PW_HTTPS_OK: return PW_STEAM_FAIL_NONE;
    case PW_HTTPS_DNS:
    case PW_HTTPS_CONNECT: return PW_STEAM_FAIL_OFFLINE;
    case PW_HTTPS_CLOCK: return PW_STEAM_FAIL_CLOCK;
    default: return PW_STEAM_FAIL_DOWNLOAD;
    }
}

int pw_steam_guide_failure_text(PwSteamFailure failure)
{
    switch (failure) {
    case PW_STEAM_FAIL_OFFLINE: return PW_TEXT_STEAM_ERR_OFFLINE;
    case PW_STEAM_FAIL_CLOCK: return PW_TEXT_STEAM_ERR_CLOCK;
    case PW_STEAM_FAIL_INSTALL: return PW_TEXT_STEAM_ERR_INSTALL;
    case PW_STEAM_FAIL_STALLED: return PW_TEXT_STEAM_ERR_STALLED;
    case PW_STEAM_FAIL_CANCELLED: return PW_TEXT_STEAM_ERR_CANCELLED;
    case PW_STEAM_FAIL_TIMEOUT: return PW_TEXT_STEAM_ERR_TIMEOUT;
    case PW_STEAM_FAIL_GUARD: return PW_TEXT_STEAM_ERR_GUARD;
    case PW_STEAM_FAIL_ENDED: return PW_TEXT_STEAM_ERR_ENDED;
    default: return PW_TEXT_STEAM_ERR_DOWNLOAD;
    }
}

static const char *const result_words[PW_STEAM_RESULTS] = {
    "none", "signed-in", "retry", "cancelled", "timeout", "stalled", "guard",
};

const char *pw_steam_guide_result_word(PwSteamResult result)
{
    return result >= 0 && result < PW_STEAM_RESULTS ? result_words[result] : "none";
}

int pw_steam_guide_record_load(PwSteamGuideRecord *out, const char *path)
{
    char text[256];
    memset(out, 0, sizeof(*out));
    FILE *file = fopen(path, "r");
    if (!file) return errno;
    const size_t size = fread(text, 1, sizeof(text), file);
    const int error = ferror(file) ? EIO : 0;
    fclose(file);
    if (error) return error;
    const char *end = text + size;
    for (const char *line = text; line < end;) {
        const char *stop = memchr(line, '\n', (size_t)(end - line));
        if (!stop) stop = end;
        size_t length = (size_t)(stop - line);
        if (length && line[length - 1] == '\r') --length;
        if (length == 13 && !memcmp(line, "stage=install", 13)) out->stage = PW_STEAM_STAGE_INSTALL;
        else if (length == 12 && !memcmp(line, "stage=signin", 12)) out->stage = PW_STEAM_STAGE_SIGNIN;
        else if (length > 7 && !memcmp(line, "result=", 7))
            for (int i = 0; i < PW_STEAM_RESULTS; ++i)
                if (strlen(result_words[i]) == length - 7 && !memcmp(line + 7, result_words[i], length - 7))
                    out->result = (PwSteamResult)i;
        line = stop + 1;
    }
    return 0;
}

int pw_steam_guide_record_save(const PwSteamGuideRecord *record, const char *path)
{
    char temporary[1024];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) return ENAMETOOLONG;
    FILE *file = fopen(temporary, "w");
    if (!file) return errno;
    int error = 0;
    const char *stage = record->stage == PW_STEAM_STAGE_INSTALL ? "install" : record->stage == PW_STEAM_STAGE_SIGNIN ? "signin" : NULL;
    if ((stage && fprintf(file, "stage=%s\n", stage) < 0) ||
        (record->result && fprintf(file, "result=%s\n", pw_steam_guide_result_word(record->result)) < 0))
        error = EIO;
    if (fflush(file) || fsync(fileno(file))) error = errno;
    if (fclose(file) && !error) error = errno;
    if (!error && rename(temporary, path)) error = errno;
    if (error) unlink(temporary);
    return error;
}

PwSteamResume pw_steam_guide_resume(const PwSteamGuideRecord *record, int installed, int signed_in,
                                    PwSteamFailure *failure)
{
    *failure = PW_STEAM_FAIL_NONE;
    if (!record || record->stage == PW_STEAM_STAGE_NONE) return PW_STEAM_RESUME_NOTHING;
    if (signed_in) return PW_STEAM_RESUME_DONE;
    if (!installed) {
        *failure = PW_STEAM_FAIL_INSTALL;
        return PW_STEAM_RESUME_FAILED;
    }
    if (record->stage == PW_STEAM_STAGE_INSTALL || record->result == PW_STEAM_RESULT_RETRY) return PW_STEAM_RESUME_SIGNIN;
    switch (record->result) {
    case PW_STEAM_RESULT_CANCELLED: *failure = PW_STEAM_FAIL_CANCELLED; break;
    case PW_STEAM_RESULT_TIMEOUT: *failure = PW_STEAM_FAIL_TIMEOUT; break;
    case PW_STEAM_RESULT_STALLED: *failure = PW_STEAM_FAIL_STALLED; break;
    case PW_STEAM_RESULT_GUARD: *failure = PW_STEAM_FAIL_GUARD; break;
    default: *failure = PW_STEAM_FAIL_ENDED; break;   /* "signed-in" with no sign-in on disk is an end too */
    }
    return PW_STEAM_RESUME_FAILED;
}

float pw_steam_guide_download_fraction(unsigned long long bytes)
{
    const float fraction = (float)bytes / (float)PW_STEAM_INSTALLER_BYTES;
    return fraction < 0.97f ? fraction : 0.97f;
}

void pw_steam_session_begin(PwSteamSession *session, int signin)
{
    memset(session, 0, sizeof(*session));
    session->signin = signin;
    session->screen = signin ? PW_STEAM_SCREEN_UPDATING : PW_STEAM_SCREEN_INSTALLING;
}

int pw_steam_session_tick(PwSteamSession *session, const PwSteamSessionInput *input)
{
    if (!session->signin) {
        session->screen = PW_STEAM_SCREEN_INSTALLING;
        return -1;
    }
    if (input->package_bytes != session->package_bytes || input->processes != session->processes) {
        session->active_at = input->seconds;
        session->package_bytes = input->package_bytes;
        session->processes = input->processes;
    }
    if (input->guard) session->guard = 1;
    if (input->signed_in) {
        /* The session ends at the sign-in and Steam is closed (the owner, 2026-10-26: a countdown over the running client
         * flickered); the title waits for the client's token before it ends the client (native/proton_wine.cpp). */
        session->signed_at = input->seconds ? input->seconds : 1;
        session->screen = PW_STEAM_SCREEN_SIGNED_IN;
        return PW_STEAM_RESULT_SIGNED_IN;
    }
    if (input->browser && !session->browser_at) session->browser_at = input->seconds ? input->seconds : 1;
    if (session->browser_at) {
        if (!session->scan_done && (input->cross || input->seconds - session->browser_at >= PW_STEAM_SCAN_SECONDS))
            session->scan_done = 1;
        session->screen = session->scan_done ? PW_STEAM_SCREEN_NONE : PW_STEAM_SCREEN_SCAN;
        return -1;
    }
    const unsigned quiet = input->seconds - session->active_at;
    if (quiet >= PW_STEAM_STALL_SECONDS) return PW_STEAM_RESULT_STALLED;
    session->screen = quiet >= PW_STEAM_SLOW_SECONDS ? PW_STEAM_SCREEN_SLOW : PW_STEAM_SCREEN_UPDATING;
    if (session->screen == PW_STEAM_SCREEN_SLOW && input->cross) return PW_STEAM_RESULT_RETRY;
    return -1;
}

PwSteamResult pw_steam_session_ended(const PwSteamSession *session, PwSteamResult why)
{
    if (session->guard && !session->signed_at &&
        (why == PW_STEAM_RESULT_NONE || why == PW_STEAM_RESULT_CANCELLED || why == PW_STEAM_RESULT_TIMEOUT))
        return PW_STEAM_RESULT_GUARD;
    return why;
}
